#include "ConfigManager.h"
#include "EPD.h"
#include "weatherIcons.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_sleep.h>
#include <sys/time.h>

// Communication and data constants
#define BAUD_RATE 115200
#define HTTP_NOT_REQUESTED_YET -999
#define STRING_BUFFER_SIZE 256
#define JSON_CAPACITY 32768

// Display and UI constants
#define TEMPERATURE_GRAPH_LINE_WIDTH 2
#define UVI_DISPLAY_THRESHOLD 4.0
#define WIND_SPEED_DISPLAY_THRESHOLD 4.0

// LED
#define PWR_LED_PIN 41

// Config button pin (GPIO 0 is typically BOOT button on ESP32 dev boards)
#define CONFIG_BUTTON_PIN 2

// Global ConfigManager instance
ConfigManager configManager;

// Forward declarations for display functions used by config portal
void displayConfigPortalScreen(const char *apName, const char *ipAddress,
                               const char *reason);

// Display buffer for config screen (shared with WeatherCrow)
uint8_t configImageBW[27200];

class WeatherCrow {
private:
  // Display buffer
  uint8_t imageBW[27200];

  // Network and API configuration (loaded from ConfigManager)
  String apiParamLatitude;
  String apiParamLongitude;
  String locationName;
  String calendarUrl;
  int refreshMinutes = DEFAULT_REFRESH_MINUTES;
  int hourInterval = DEFAULT_HOUR_INTERVAL;
  bool lowPowerMode = DEFAULT_LOW_POWER_MODE;
  String units = "metric";
  String timeFormat = "24h";
  bool enableAlertDisplay = true;
  String alertRegion = "auto";
  String geoCountryCode;
  bool usingManualCoordinates = false;

  static const size_t MAX_CALENDAR_EVENTS = 8;
  struct CalendarEvent {
    String date;
    String time;
    String title;
  };
  CalendarEvent calendarEvents[MAX_CALENDAR_EVENTS];
  size_t calendarEventCount = 0;

  // Data buffers
  String jsonBuffer;
  String errorMessageBuffer;
  int httpResponseCode = HTTP_NOT_REQUESTED_YET;
  JsonDocument weatherApiResponse;
  bool ledState = false;

  static const size_t DAILY_FORECAST_DAYS = 5;
  struct DailySummary {
    bool valid = false;
    long dateTime = 0;
    int weatherCode = 0;
    int tempMax = 0;
    int tempMin = 0;
    int precipProb = 0;
    float precipMm = 0.0f;
    String condition;
  };
  DailySummary dailySummary[DAILY_FORECAST_DAYS]; // [0]=Today .. [4]=+4 days
  String lastHttpDateHeader;

  /**
   * Weather information structure to store processed data from API
   */
  struct WeatherInfo {
    String weather;
    long currentDateTime = 0;
    long refreshDateTime = 0;
    long sunrise = 0;
    long sunset = 0;
    String temperature;
    String tempIntegerPart;
    String tempDecimalPart;
    String humidity;
    String pressure;
    String windSpeed;
    String timezone;
    String icon;
  } weatherInfo;

  // Logging helper methods
  void logPrint(const char *msg) { Serial.print(msg); }
  void logPrint(int msg) { Serial.print(msg); }
  void logPrint(String msg) { Serial.print(msg); }

  void logPrintln(const char *msg) { Serial.println(msg); }
  void logPrintln(int msg) { Serial.println(msg); }
  void logPrintln(String msg) { Serial.println(msg); }

  /**
   * Performs HTTPS GET request to the specified server
   */
  String httpsGETRequest(const char *serverName,
                         const char *acceptHeader = nullptr) {
    WiFiClientSecure client;
    HTTPClient http;
    client.setInsecure();
    http.setTimeout(10000);
    http.setUserAgent(
        "WeatherCrow/1.0 (https://github.com/kotamorishi/weather-crow5.7)");
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    http.begin(client, serverName);
    if (acceptHeader != nullptr) {
      http.addHeader("Accept", acceptHeader);
    }
    const char *headerKeys[] = {"Date"};
    http.collectHeaders(headerKeys, 1);
    lastHttpDateHeader = "";
    httpResponseCode = http.GET();
    String payload = "{}";
    if (httpResponseCode > 0) {
      if (http.hasHeader("Date")) {
        lastHttpDateHeader = http.header("Date");
      }
      payload = http.getString();
    }
    http.end();
    return payload;
  }

  /**
   * Performs HTTPS GET request and streams JSON directly into outDoc with an
   * optional ArduinoJson filter (uses HTTP/1.0 to avoid chunked framing).
   * Saves RAM on large GeoJSON responses by skipping unused geometry fields.
   */
  bool httpsGETJsonFiltered(const char *serverName, const JsonDocument *filter,
                            JsonDocument &outDoc,
                            const char *acceptHeader = nullptr) {
    WiFiClientSecure client;
    HTTPClient http;
    client.setInsecure();
    http.useHTTP10(true);
    http.setTimeout(10000);
    http.setUserAgent(
        "WeatherCrow/1.0 (https://github.com/kotamorishi/weather-crow5.7)");
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    http.begin(client, serverName);
    if (acceptHeader != nullptr) {
      http.addHeader("Accept", acceptHeader);
    }

    httpResponseCode = http.GET();
    if (httpResponseCode != HTTP_CODE_OK) {
      http.end();
      return false;
    }

    DeserializationError err =
        (filter != nullptr)
            ? deserializeJson(outDoc, http.getStream(),
                              DeserializationOption::Filter(*filter))
            : deserializeJson(outDoc, http.getStream());
    http.end();

    if (err) {
      logPrint("JSON stream parse error: ");
      logPrintln(err.c_str());
      return false;
    }
    return true;
  }

  /**
   * Load configuration from ConfigManager
   */
  void loadConfiguration() {
    usingManualCoordinates = configManager.hasManualCoordinates();
    apiParamLatitude = configManager.getLatitude();
    apiParamLongitude = configManager.getLongitude();
    locationName = configManager.getLocationName();
    geoCountryCode = configManager.getGeoCountryCode();
    calendarUrl = configManager.getCalendarUrl();
    refreshMinutes = configManager.getRefreshMinutes();
    hourInterval = configManager.getHourInterval();
    lowPowerMode = configManager.getLowPowerMode();
    units = configManager.getUnits();
    timeFormat = configManager.getTimeFormat();
    enableAlertDisplay = configManager.getEnableAlertDisplay();
    alertRegion = configManager.getAlertRegion();

    logPrintln("Configuration loaded:");
    logPrint("  Location: ");
    logPrintln(locationName);
    logPrint("  Coordinates (");
    logPrint(usingManualCoordinates ? "manual" : "auto/GeoIP");
    logPrint("): ");
    logPrint(apiParamLatitude);
    logPrint(", ");
    logPrintln(apiParamLongitude);
    logPrint("  Alert Region: ");
    logPrintln(alertRegion);
    logPrint("  Calendar URL: ");
    logPrintln(calendarUrl.length() > 0 ? "(set)" : "(not set)");
    logPrint("  Refresh: ");
    logPrint(refreshMinutes);
    logPrintln(" minutes");
  }

  /**
   * Resolves coordinates via GeoJS (https://get.geojs.io/v1/ip/geo.json) when
   * manual latitude/longitude are not configured. Results are cached in NVS
   * per WiFi SSID so deep-sleep wakes avoid redundant network calls.
   * Manual coordinates in the config portal always override GeoJS.
   */
  bool resolveLocationWithGeoIp() {
    if (usingManualCoordinates) {
      logPrintln("Using manual coordinates override from config portal.");
      return true;
    }

    String currentSsid = WiFi.SSID();
    if (configManager.hasGeoCache() &&
        configManager.getGeoSsid() == currentSsid) {
      apiParamLatitude = configManager.getGeoLatitude();
      apiParamLongitude = configManager.getGeoLongitude();
      geoCountryCode = configManager.getGeoCountryCode();
      if (locationName.length() == 0) {
        locationName = configManager.getGeoCity();
      }
      logPrint("Using cached GeoJS location: ");
      logPrint(locationName);
      logPrint(", ");
      logPrint(geoCountryCode);
      logPrint(" (");
      logPrint(apiParamLatitude);
      logPrint(", ");
      logPrint(apiParamLongitude);
      logPrintln(")");
      return true;
    }

    logPrintln(
        "Fetching location via GeoJS (https://get.geojs.io/v1/ip/geo.json)...");
    JsonDocument filter;
    filter["latitude"] = true;
    filter["longitude"] = true;
    filter["city"] = true;
    filter["country_code"] = true;

    JsonDocument geoDoc;
    bool ok = httpsGETJsonFiltered("https://get.geojs.io/v1/ip/geo.json",
                                   &filter, geoDoc, "application/json");
    if (ok) {
      String lat = geoDoc["latitude"] | "";
      String lon = geoDoc["longitude"] | "";
      String city = geoDoc["city"] | "";
      String cc = geoDoc["country_code"] | "";
      lat.trim();
      lon.trim();
      city.trim();
      cc.trim();
      cc.toUpperCase();

      if (lat.length() > 0 && lon.length() > 0) {
        configManager.saveGeoIpCache(lat, lon, city, cc, currentSsid);
        apiParamLatitude = lat;
        apiParamLongitude = lon;
        geoCountryCode = cc;
        if (locationName.length() == 0) {
          locationName = city;
        }
        logPrint("GeoJS resolved & cached: ");
        logPrint(city);
        logPrint(", ");
        logPrint(cc);
        logPrint(" (");
        logPrint(lat);
        logPrint(", ");
        logPrint(lon);
        logPrintln(")");
        return true;
      }
    }

    // Fallback to stale cache if GeoJS is temporarily unreachable
    if (configManager.hasGeoCache()) {
      logPrintln("GeoJS lookup failed; falling back to previously cached "
                 "coordinates.");
      apiParamLatitude = configManager.getGeoLatitude();
      apiParamLongitude = configManager.getGeoLongitude();
      geoCountryCode = configManager.getGeoCountryCode();
      if (locationName.length() == 0) {
        locationName = configManager.getGeoCity();
      }
      return true;
    }

    errorMessageBuffer =
        "Auto location lookup (GeoJS) failed. Hold config button on boot to "
        "enter latitude/longitude manually.";
    return false;
  }

  void screenPowerOn() {
    const int SCREEN_POWER_PIN = 7;
    pinMode(SCREEN_POWER_PIN, OUTPUT);
    digitalWrite(SCREEN_POWER_PIN, HIGH);
  }

  void clearScreen() {
    Paint_NewImage(imageBW, EPD_W, EPD_H, Rotation, WHITE);
    Paint_Clear(WHITE);
    EPD_Init();
  }

  /**
   * Safely retrieves an icon from the icon map, returning a fallback if not
   * found
   * @param iconKey The key to look up in icon_map
   * @param fallbackKey Optional fallback key if the main key is not found
   * (defaults to "error_sm")
   * @return Reference to the icon data, or fallback icon if not found
   */
  const unsigned char *getIcon(const char *iconKey,
                               const char *fallbackKey = "na_md") {
    // First check if the requested icon exists
    if (icon_map.count(iconKey) > 0) {
      return icon_map[iconKey];
    }

    // If not found, try to use the fallback
    if (icon_map.count(fallbackKey) > 0) {
      Serial.print("Icon not found: ");
      Serial.print(iconKey);
      Serial.print(", using fallback: ");
      Serial.println(fallbackKey);
      return icon_map[fallbackKey];
    }

    // If even fallback isn't available, return the first available icon
    Serial.print("Neither requested icon nor fallback found: ");
    Serial.println(iconKey);
    return icon_map.begin()->second;
  }

  /**
   * Displays system information on the screen
   */
  void displaySystemInfo(uint16_t baseX, uint16_t baseY) {
    uint16_t x = baseX;
    uint16_t y = baseY;
    uint16_t yOffset = 8;
    char buffer[STRING_BUFFER_SIZE];

    // Display WiFi SSID (show current connected network)
    memset(buffer, 0, sizeof(buffer));
    snprintf(buffer, sizeof(buffer), "WIFI: %s", WiFi.SSID().c_str());
    EPD_ShowString(x, y, buffer, FONT_SIZE_8, BLACK, false);
    y += yOffset;

    // Display WiFi connection status
    memset(buffer, 0, sizeof(buffer));
    if (WL_CONNECTED == WiFi.status()) {
      snprintf(buffer, sizeof(buffer), "DEVICE IS CONNECTED TO WIFI. IP: %s",
               WiFi.localIP().toString().c_str());
    } else {
      snprintf(buffer, sizeof(buffer),
               "WIFI NOT CONNECTED. CHECK WIFI SSID AND CREDENTIALS. ROUTER "
               "REBOOT MAY HELP.");
    }
    EPD_ShowString(x, y, buffer, FONT_SIZE_8, BLACK, false);
    y += yOffset;

    // Display location coordinates
    memset(buffer, 0, sizeof(buffer));
    snprintf(buffer, sizeof(buffer), "LOCATION LATITUDE: %s, LONGITUDE: %s",
             apiParamLatitude.c_str(), apiParamLongitude.c_str());
    EPD_ShowString(x, y, buffer, FONT_SIZE_8, BLACK, false);
    y += yOffset;

    // Display latest API call time
    memset(buffer, 0, sizeof(buffer));
    snprintf(
        buffer, sizeof(buffer), "LATEST SUCCESSFUL API CALL: %s",
        convertUnixTimeToDateTimeString(weatherInfo.currentDateTime).c_str());
    EPD_ShowString(x, y, buffer, FONT_SIZE_8, BLACK, false);
  }

  /**
   * Converts Unix timestamp to formatted date-time string
   */
  String convertUnixTimeToDateTimeString(long unixTime) {
    if (unixTime == 0) {
      return "N/A";
    }

    time_t rawtime = unixTime;
    struct tm *dt;
    char buffer[30];
    dt = gmtime(&rawtime);
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", dt);
    return String(buffer);
  }

  /**
   * Converts Unix timestamp to specified formatted date-time string
   */
  String convertUnixTimeToSpecifiedDateTimeString(long unixTime,
                                                  const char *format) {
    if (unixTime == 0) {
      return "N/A";
    }

    time_t rawtime = unixTime;
    struct tm *dt;
    char buffer[30];
    dt = gmtime(&rawtime);
    strftime(buffer, sizeof(buffer), format, dt);
    return String(buffer);
  }

  /**
   * Converts Unix timestamp to hour:minute format honoring 12h (AM/PM) or 24h
   */
  String convertUnixTimeToTimeString(long unixTime) {
    if (unixTime == 0) {
      return "N/A";
    }

    time_t rawtime = unixTime;
    struct tm *dt = gmtime(&rawtime);
    char buffer[30];
    if (timeFormat == "24h") {
      strftime(buffer, sizeof(buffer), "%H:%M", dt);
    } else {
      strftime(buffer, sizeof(buffer), "%I:%M %p", dt);
      if (buffer[0] == '0') {
        memmove(buffer, buffer + 1, strlen(buffer));
      }
    }
    return String(buffer);
  }

  /**
   * Converts Unix timestamp to short time format ("3 PM" in 12h, "15:00" in
   * 24h)
   */
  String convertUnixTimeToShortDateTimeString(long unixTime) {
    if (unixTime == 0) {
      return "N/A";
    }

    time_t rawtime = unixTime;
    struct tm *dt;
    char buffer[30];
    dt = gmtime(&rawtime);
    if (timeFormat == "24h") {
      strftime(buffer, sizeof(buffer), "%H:00", dt);
    } else {
      strftime(buffer, sizeof(buffer), "%I %p", dt);
      // Remove leading zero from hour
      if (buffer[0] == '0') {
        memmove(buffer, buffer + 1, strlen(buffer));
      }
    }
    return String(buffer);
  }

  /**
   * Converts Unix timestamp to display format for date
   */
  String convertUnixTimeToDisplayFormat(long unixTime) {
    if (unixTime == 0) {
      return "N/A";
    }

    char buffer[30];
    String monthStr = convertUnixTimeToSpecifiedDateTimeString(unixTime, "%b");
    String dayStr = convertUnixTimeToSpecifiedDateTimeString(unixTime, "%d");
    dayStr = formatDay(dayStr);
    String weekOfDayStr =
        convertUnixTimeToSpecifiedDateTimeString(unixTime, "%a");
    snprintf(buffer, sizeof(buffer), "%s %s, %s", monthStr.c_str(),
             dayStr.c_str(), weekOfDayStr.c_str());
    return String(buffer);
  }

  /**
   * Formats a date string by removing leading zeros and adding suffix for 1st,
   * 2nd, 3rd days
   * @param dateStr Date string (e.g., "01", "08")
   * @return Formatted date string (e.g., "1", "2", "3", "4", "5", etc.)
   */
  String formatDay(String dateStr) {
    int day = dateStr.toInt();
    return String(day);
  }

  /**
   * Displays an error message on the screen
   */
  void displayErrorMessage(char *title, char *description) {
    clearScreen();

    uint16_t baseXpos = 258;
    char buffer[STRING_BUFFER_SIZE];

    // Display error icon - with safety check
    const char *icons[] = {"emma_cupcake_lg", "emma_mon1_lg", "leo_face_lg",
                           "leo_gator_lg"};
    int randomIndex = random(0, sizeof(icons) / sizeof(icons[0]));
    const char *selectedIcon = icons[randomIndex];

    // Safely get the icon
    EPD_drawImage(30, 20, getIcon(selectedIcon));
    EPD_DrawLine(baseXpos, 110, 740, 110, BLACK);

    // Display error title
    memset(buffer, 0, sizeof(buffer));
    snprintf(buffer, sizeof(buffer), "%s ", title);
    EPD_ShowString(baseXpos, 70, buffer, FONT_SIZE_36, BLACK);

    // Display error description
    memset(buffer, 0, sizeof(buffer));
    snprintf(buffer, sizeof(buffer), "%s ", description);
    EPD_ShowString(baseXpos, 140, buffer, FONT_SIZE_16, BLACK);

    // Display system information
    displaySystemInfo(baseXpos, 230);

    // Update display and enter deep sleep
    EPD_Display(imageBW);
    EPD_Update();
    EPD_DeepSleep();
  }

  String constructApiEndpointUrl() {
    String tempUnit = (units == "imperial") ? "fahrenheit" : "celsius";
    return "https://api.open-meteo.com/v1/forecast?latitude=" +
           apiParamLatitude + "&longitude=" + apiParamLongitude +
           "&current=temperature_2m,relative_humidity_2m,surface_pressure,"
           "wind_speed_10m,weather_code,is_day,rain,snowfall,uv_index"
           "&hourly=precipitation_probability,weather_code"
           "&daily=weather_code,temperature_2m_max,temperature_2m_min,"
           "precipitation_probability_max,precipitation_sum,sunrise,sunset"
           "&temperature_unit=" +
           tempUnit +
           "&wind_speed_unit=ms&precipitation_unit=mm"
           "&timeformat=unixtime&timezone=auto&forecast_days=5";
  }

  /**
   * Formats a coordinate string to at most 4 decimal places (stripping trailing
   * zeros) as required by the US National Weather Service API.
   */
  String formatCoordForNws(const String &coord) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%.4f", coord.toFloat());
    String formatted(buf);
    if (formatted.indexOf('.') != -1) {
      while (formatted.endsWith("0")) {
        formatted.remove(formatted.length() - 1);
      }
      if (formatted.endsWith(".")) {
        formatted.remove(formatted.length() - 1);
      }
    }
    return formatted;
  }

  /**
   * Maps Open-Meteo WMO weather interpretation codes (0-99) and is_day flag
   * to the built-in icon code ("01d", "02n", etc.).
   */
  String wmoCodeToIcon(int code, int isDay) {
    const char *suffix = (isDay != 0) ? "d" : "n";
    const char *prefix = "01";
    switch (code) {
    case 0:
      prefix = "01"; // Clear sky
      break;
    case 1:
      prefix = "02"; // Mainly clear
      break;
    case 2:
      prefix = "03"; // Partly cloudy
      break;
    case 3:
      prefix = "04"; // Overcast
      break;
    case 45:
    case 48:
      prefix = "50"; // Fog / depositing rime fog
      break;
    case 51:
    case 53:
    case 55:
    case 56:
    case 57:
    case 80:
    case 81:
    case 82:
      prefix = "09"; // Drizzle / Rain showers
      break;
    case 61:
    case 63:
    case 65:
      prefix = "10"; // Rain
      break;
    case 66:
    case 67:
    case 71:
    case 73:
    case 75:
    case 77:
    case 85:
    case 86:
      prefix = "13"; // Freezing rain / Snow
      break;
    case 95:
    case 96:
    case 99:
      prefix = "11"; // Thunderstorm
      break;
    default:
      prefix = "01";
      break;
    }
    return String(prefix) + String(suffix);
  }

  /**
   * Maps Open-Meteo WMO weather code to a short condition name
   */
  String wmoCodeToWeatherMain(int code) {
    switch (code) {
    case 0:
      return "Clear";
    case 1:
    case 2:
    case 3:
      return "Clouds";
    case 45:
    case 48:
      return "Fog";
    case 51:
    case 53:
    case 55:
    case 56:
    case 57:
      return "Drizzle";
    case 61:
    case 63:
    case 65:
    case 66:
    case 67:
    case 80:
    case 81:
    case 82:
      return "Rain";
    case 71:
    case 73:
    case 75:
    case 77:
    case 85:
    case 86:
      return "Snow";
    case 95:
    case 96:
    case 99:
      return "Thunderstorm";
    default:
      return "Clear";
    }
  }

  /**
   * Parses an ISO-8601 timestamp string (e.g., "2026-11-04T22:00:00-05:00" or
   * "2026-11-20T23:00:00.000Z") into a local Unix timestamp adjusted by
   * localUtcOffsetSeconds.
   */
  long parseIso8601ToLocalUnixTime(const String &isoStr,
                                   long localUtcOffsetSeconds) {
    if (isoStr.length() < 19)
      return 0;
    int y = 0, m = 0, d = 0, hh = 0, mm = 0, ss = 0;
    if (sscanf(isoStr.c_str(), "%d-%d-%dT%d:%d:%d", &y, &m, &d, &hh, &mm,
               &ss) != 6) {
      return 0;
    }

    int yr = y - (m <= 2);
    int era = (yr >= 0 ? yr : yr - 399) / 400;
    unsigned yoe = static_cast<unsigned>(yr - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = era * 146097L + static_cast<long>(doe) - 719468L;
    long timestamp = days * 86400L + hh * 3600L + mm * 60L + ss;

    long isoOffsetSeconds = 0;
    size_t tzPos = 19;
    if (tzPos < isoStr.length() && isoStr.charAt(tzPos) == '.') {
      tzPos++;
      while (tzPos < isoStr.length() && isDigit(isoStr.charAt(tzPos))) {
        tzPos++;
      }
    }
    if (tzPos < isoStr.length()) {
      char sign = isoStr.charAt(tzPos);
      if ((sign == '+' || sign == '-') && tzPos + 5 < isoStr.length()) {
        int offH = 0, offM = 0;
        if (sscanf(isoStr.c_str() + tzPos + 1, "%d:%d", &offH, &offM) == 2) {
          isoOffsetSeconds =
              (offH * 3600L + offM * 60L) * (sign == '-' ? -1L : 1L);
        }
      }
    }
    long utcTime = timestamp - isoOffsetSeconds;
    return utcTime + localUtcOffsetSeconds;
  }

  /**
   * Parses an RFC-7231 HTTP Date header (e.g., "Fri, 20 Nov 2026 20:29:30 GMT")
   * into a local Unix timestamp adjusted by localUtcOffsetSeconds.
   */
  long parseHttpDateToLocalUnixTime(const String &dateHeader,
                                    long localUtcOffsetSeconds) {
    if (dateHeader.length() < 25)
      return 0;
    char dow[8] = {0}, monStr[8] = {0};
    int d = 0, y = 0, hh = 0, mm = 0, ss = 0;
    if (sscanf(dateHeader.c_str(), "%7[^,], %d %7s %d %d:%d:%d", dow, &d,
               monStr, &y, &hh, &mm, &ss) != 7) {
      return 0;
    }

    static const char *const months[12] = {"Jan", "Feb", "Mar", "Apr",
                                           "May", "Jun", "Jul", "Aug",
                                           "Sep", "Oct", "Nov", "Dec"};
    int m = 0;
    for (int i = 0; i < 12; i++) {
      if (strncasecmp(monStr, months[i], 3) == 0) {
        m = i + 1;
        break;
      }
    }
    if (m == 0)
      return 0;

    int yr = y - (m <= 2);
    int era = (yr >= 0 ? yr : yr - 399) / 400;
    unsigned yoe = static_cast<unsigned>(yr - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = era * 146097L + static_cast<long>(doe) - 719468L;
    long utcTime = days * 86400L + hh * 3600L + mm * 60L + ss;
    return utcTime + localUtcOffsetSeconds;
  }

  /**
   * Infers a supported alert country code ("US", "CA", "NO", "DE") from the
   * IANA timezone string returned by Open-Meteo for the requested coordinates.
   */
  String inferAlertRegionFromTimezone(const String &tz) {
    if (tz == "Europe/Oslo" || tz == "Arctic/Longyearbyen") {
      return "NO";
    }
    if (tz == "Europe/Berlin" || tz == "Europe/Busingen") {
      return "DE";
    }

    static const char *const caZones[] = {
        "America/Toronto",      "America/Vancouver",
        "America/Edmonton",     "America/Winnipeg",
        "America/Halifax",      "America/St_Johns",
        "America/Regina",       "America/Moncton",
        "America/Glace_Bay",    "America/Goose_Bay",
        "America/Blanc-Sablon", "America/Montreal",
        "America/Iqaluit",      "America/Rankin_Inlet",
        "America/Resolute",     "America/Cambridge_Bay",
        "America/Yellowknife",  "America/Inuvik",
        "America/Whitehorse",   "America/Dawson",
        "America/Dawson_Creek", "America/Fort_Nelson",
        "America/Creston",      "America/Swift_Current",
        "America/Atikokan",     "America/Thunder_Bay",
        "America/Nipigon",      "America/Rainy_River",
        "America/Pangnirtung"};
    for (const char *z : caZones) {
      if (tz == z)
        return "CA";
    }

    if (tz.startsWith("America/Indiana/") ||
        tz.startsWith("America/Kentucky/") ||
        tz.startsWith("America/North_Dakota/")) {
      return "US";
    }
    static const char *const usZones[] = {
        "America/New_York",   "America/Detroit",     "America/Chicago",
        "America/Denver",     "America/Los_Angeles", "America/Phoenix",
        "America/Anchorage",  "America/Juneau",      "America/Sitka",
        "America/Metlakatla", "America/Yakutat",     "America/Nome",
        "America/Adak",       "America/Boise",       "America/Menominee",
        "Pacific/Honolulu",   "America/Puerto_Rico"};
    for (const char *z : usZones) {
      if (tz == z)
        return "US";
    }

    return "";
  }

  /**
   * Resolves which regional alert provider to query ("US", "CA", "NO", "DE",
   * or "" if no supported provider matches).
   */
  String resolveAlertRegion() {
    if (alertRegion.length() > 0 && alertRegion != "auto") {
      return alertRegion;
    }

    // When using GeoJS coordinates directly, trust GeoJS country_code first
    if (!usingManualCoordinates && geoCountryCode.length() == 2) {
      return geoCountryCode;
    }

    // For manual coordinates (e.g., testing another country or VPN), infer
    // from Open-Meteo's coordinate-based timezone first
    String tzRegion = inferAlertRegionFromTimezone(weatherInfo.timezone);
    if (tzRegion.length() == 2) {
      return tzRegion;
    }

    // Fallback to cached GeoJS country code if manual coordinates are nearby
    if (geoCountryCode.length() == 2 && configManager.hasGeoCache()) {
      float dLat = fabsf(apiParamLatitude.toFloat() -
                         configManager.getGeoLatitude().toFloat());
      float dLon = fabsf(apiParamLongitude.toFloat() -
                         configManager.getGeoLongitude().toFloat());
      if (dLat < 1.5f && dLon < 1.5f) {
        return geoCountryCode;
      }
    }

    return "";
  }

  /**
   * Fetches active weather alerts from the US National Weather Service API
   * (api.weather.gov) and normalizes them into weatherApiResponse["alerts"].
   */
  void fetchNwsAlerts() {
    String nwsUrl = "https://api.weather.gov/alerts/active?point=" +
                    formatCoordForNws(apiParamLatitude) + "," +
                    formatCoordForNws(apiParamLongitude);

    logPrint("Fetching US NWS alerts: ");
    logPrintln(nwsUrl);

    JsonDocument filter;
    JsonObject propsFilter =
        filter["features"][0]["properties"].to<JsonObject>();
    propsFilter["event"] = true;
    propsFilter["senderName"] = true;
    propsFilter["onset"] = true;
    propsFilter["effective"] = true;
    propsFilter["ends"] = true;
    propsFilter["expires"] = true;
    propsFilter["description"] = true;
    propsFilter["headline"] = true;

    JsonDocument nwsDoc;
    if (!httpsGETJsonFiltered(nwsUrl.c_str(), &filter, nwsDoc,
                              "application/geo+json")) {
      logPrint("NWS alerts fetch/parse failed (HTTP ");
      logPrint(httpResponseCode);
      logPrintln(")");
      return;
    }

    JsonArray features = nwsDoc["features"].as<JsonArray>();
    if (features.isNull() || features.size() == 0) {
      logPrintln("No active US NWS weather alerts.");
      return;
    }

    long utcOffset = weatherApiResponse["utc_offset_seconds"].as<long>();
    JsonArray alertsArray = weatherApiResponse["alerts"].to<JsonArray>();

    for (JsonObject feature : features) {
      JsonObject props = feature["properties"];
      if (props.isNull())
        continue;

      JsonObject alertObj = alertsArray.add<JsonObject>();
      alertObj["event"] = props["event"] | "Weather Alert";
      alertObj["sender_name"] = props["senderName"] | "NWS";

      String startIso = !props["onset"].isNull()
                            ? props["onset"].as<String>()
                            : props["effective"].as<String>();
      String endIso = !props["ends"].isNull() ? props["ends"].as<String>()
                                              : props["expires"].as<String>();

      long startUnix = parseIso8601ToLocalUnixTime(startIso, utcOffset);
      long endUnix = parseIso8601ToLocalUnixTime(endIso, utcOffset);
      if (startUnix > 0)
        alertObj["start"] = startUnix;
      if (endUnix > 0)
        alertObj["end"] = endUnix;

      String desc = !props["description"].isNull()
                        ? props["description"].as<String>()
                        : props["headline"].as<String>();
      alertObj["description"] = desc;
    }

    logPrint("Loaded US NWS alerts count: ");
    logPrintln((int)alertsArray.size());
  }

  /**
   * Fetches active weather alerts from Environment and Climate Change Canada
   * (api.weather.gc.ca) using a point bounding box and skipGeometry=true.
   */
  void fetchEcccAlerts() {
    float lat = apiParamLatitude.toFloat();
    float lon = apiParamLongitude.toFloat();
    const float delta = 0.01f;
    char bboxBuf[64];
    snprintf(bboxBuf, sizeof(bboxBuf), "%.4f,%.4f,%.4f,%.4f", lon - delta,
             lat - delta, lon + delta, lat + delta);

    String url =
        "https://api.weather.gc.ca/collections/weather-alerts/items?f=json"
        "&skipGeometry=true&limit=5&bbox=" +
        String(bboxBuf);

    logPrint("Fetching Canada ECCC alerts: ");
    logPrintln(url);

    JsonDocument filter;
    JsonObject propsFilter =
        filter["features"][0]["properties"].to<JsonObject>();
    propsFilter["alert_name_en"] = true;
    propsFilter["alert_short_name_en"] = true;
    propsFilter["status_en"] = true;
    propsFilter["publication_datetime"] = true;
    propsFilter["validity_datetime"] = true;
    propsFilter["event_end_datetime"] = true;
    propsFilter["expiration_datetime"] = true;
    propsFilter["alert_text_en"] = true;

    JsonDocument doc;
    if (!httpsGETJsonFiltered(url.c_str(), &filter, doc, "application/json")) {
      logPrint("ECCC alerts fetch/parse failed (HTTP ");
      logPrint(httpResponseCode);
      logPrintln(")");
      return;
    }

    JsonArray features = doc["features"].as<JsonArray>();
    if (features.isNull() || features.size() == 0) {
      logPrintln("No active Canada ECCC weather alerts.");
      return;
    }

    long utcOffset = weatherApiResponse["utc_offset_seconds"].as<long>();
    JsonArray alertsArray = weatherApiResponse["alerts"].to<JsonArray>();

    for (JsonObject feature : features) {
      JsonObject props = feature["properties"];
      if (props.isNull())
        continue;

      String status = props["status_en"] | "";
      status.toLowerCase();
      if (status == "ended")
        continue;

      JsonObject alertObj = alertsArray.add<JsonObject>();
      String eventName = !props["alert_name_en"].isNull()
                             ? props["alert_name_en"].as<String>()
                             : (props["alert_short_name_en"] | "Weather Alert");
      alertObj["event"] = eventName;
      alertObj["sender_name"] = "Environment Canada";

      String startIso = !props["validity_datetime"].isNull()
                            ? props["validity_datetime"].as<String>()
                            : props["publication_datetime"].as<String>();
      String endIso = !props["event_end_datetime"].isNull()
                          ? props["event_end_datetime"].as<String>()
                          : props["expiration_datetime"].as<String>();

      long startUnix = parseIso8601ToLocalUnixTime(startIso, utcOffset);
      long endUnix = parseIso8601ToLocalUnixTime(endIso, utcOffset);
      if (startUnix > 0)
        alertObj["start"] = startUnix;
      if (endUnix > 0)
        alertObj["end"] = endUnix;

      alertObj["description"] = props["alert_text_en"] | "";
    }

    if (alertsArray.size() == 0) {
      weatherApiResponse.remove("alerts");
    }

    logPrint("Loaded Canada ECCC alerts count: ");
    logPrintln((int)alertsArray.size());
  }

  /**
   * Fetches active weather alerts from MET Norway MetAlerts 2.0
   * (api.met.no/weatherapi/metalerts/2.0/current.json).
   */
  void fetchMetNoAlerts() {
    String url = "https://api.met.no/weatherapi/metalerts/2.0/"
                 "current.json?lang=en&lat=" +
                 apiParamLatitude + "&lon=" + apiParamLongitude;

    logPrint("Fetching MET Norway alerts: ");
    logPrintln(url);

    JsonDocument filter;
    JsonObject propsFilter =
        filter["features"][0]["properties"].to<JsonObject>();
    propsFilter["event"] = true;
    propsFilter["eventAwarenessName"] = true;
    propsFilter["description"] = true;
    propsFilter["title"] = true;
    propsFilter["status"] = true;
    filter["features"][0]["when"]["interval"] = true;

    JsonDocument doc;
    if (!httpsGETJsonFiltered(url.c_str(), &filter, doc, "application/json")) {
      logPrint("MET Norway alerts fetch/parse failed (HTTP ");
      logPrint(httpResponseCode);
      logPrintln(")");
      return;
    }

    JsonArray features = doc["features"].as<JsonArray>();
    if (features.isNull() || features.size() == 0) {
      logPrintln("No active MET Norway weather alerts.");
      return;
    }

    long utcOffset = weatherApiResponse["utc_offset_seconds"].as<long>();
    JsonArray alertsArray = weatherApiResponse["alerts"].to<JsonArray>();

    for (JsonObject feature : features) {
      JsonObject props = feature["properties"];
      if (props.isNull())
        continue;

      String status = props["status"] | "Actual";
      if (status.length() > 0 && status != "Actual")
        continue;

      JsonObject alertObj = alertsArray.add<JsonObject>();
      String eventName = !props["eventAwarenessName"].isNull()
                             ? props["eventAwarenessName"].as<String>()
                             : (props["event"] | "Weather Alert");
      alertObj["event"] = eventName;
      alertObj["sender_name"] = "MET Norway";

      JsonArray interval = feature["when"]["interval"].as<JsonArray>();
      if (!interval.isNull() && interval.size() >= 1) {
        long startUnix =
            parseIso8601ToLocalUnixTime(interval[0].as<String>(), utcOffset);
        if (startUnix > 0)
          alertObj["start"] = startUnix;
      }
      if (!interval.isNull() && interval.size() >= 2) {
        long endUnix =
            parseIso8601ToLocalUnixTime(interval[1].as<String>(), utcOffset);
        if (endUnix > 0)
          alertObj["end"] = endUnix;
      }

      String desc = !props["description"].isNull()
                        ? props["description"].as<String>()
                        : (props["title"] | "");
      alertObj["description"] = desc;
    }

    if (alertsArray.size() == 0) {
      weatherApiResponse.remove("alerts");
    }

    logPrint("Loaded MET Norway alerts count: ");
    logPrintln((int)alertsArray.size());
  }

  /**
   * Fetches active German DWD weather alerts via Bright Sky
   * (api.brightsky.dev/alerts).
   */
  void fetchBrightSkyAlerts() {
    String url = "https://api.brightsky.dev/alerts?lat=" + apiParamLatitude +
                 "&lon=" + apiParamLongitude;

    logPrint("Fetching Bright Sky (DWD) alerts: ");
    logPrintln(url);

    JsonDocument filter;
    JsonObject alertFilter = filter["alerts"][0].to<JsonObject>();
    alertFilter["event_en"] = true;
    alertFilter["event_de"] = true;
    alertFilter["headline_en"] = true;
    alertFilter["headline_de"] = true;
    alertFilter["description_en"] = true;
    alertFilter["description_de"] = true;
    alertFilter["onset"] = true;
    alertFilter["effective"] = true;
    alertFilter["expires"] = true;

    JsonDocument doc;
    if (!httpsGETJsonFiltered(url.c_str(), &filter, doc, "application/json")) {
      logPrint("Bright Sky alerts fetch/parse failed (HTTP ");
      logPrint(httpResponseCode);
      logPrintln(")");
      return;
    }

    JsonArray alerts = doc["alerts"].as<JsonArray>();
    if (alerts.isNull() || alerts.size() == 0) {
      logPrintln("No active Bright Sky (DWD) weather alerts.");
      return;
    }

    long utcOffset = weatherApiResponse["utc_offset_seconds"].as<long>();
    JsonArray alertsArray = weatherApiResponse["alerts"].to<JsonArray>();

    for (JsonObject item : alerts) {
      JsonObject alertObj = alertsArray.add<JsonObject>();

      String eventName = !item["event_en"].isNull()
                             ? item["event_en"].as<String>()
                             : (item["event_de"] | "Weather Alert");
      alertObj["event"] = eventName;
      alertObj["sender_name"] = "DWD";

      String startIso = !item["onset"].isNull()
                            ? item["onset"].as<String>()
                            : item["effective"].as<String>();
      String endIso =
          !item["expires"].isNull() ? item["expires"].as<String>() : String("");

      long startUnix = parseIso8601ToLocalUnixTime(startIso, utcOffset);
      long endUnix = parseIso8601ToLocalUnixTime(endIso, utcOffset);
      if (startUnix > 0)
        alertObj["start"] = startUnix;
      if (endUnix > 0)
        alertObj["end"] = endUnix;

      String desc = "";
      if (!item["description_en"].isNull()) {
        desc = item["description_en"].as<String>();
      } else if (!item["headline_en"].isNull()) {
        desc = item["headline_en"].as<String>();
      } else if (!item["description_de"].isNull()) {
        desc = item["description_de"].as<String>();
      } else if (!item["headline_de"].isNull()) {
        desc = item["headline_de"].as<String>();
      }
      alertObj["description"] = desc;
    }

    logPrint("Loaded Bright Sky (DWD) alerts count: ");
    logPrintln((int)alertsArray.size());
  }

  /**
   * Dispatches regional weather alert fetching based on configured or
   * auto-detected region (US NWS, Canada ECCC, Norway MET, Germany DWD).
   * Non-fatal: logs and continues without alerts on unsupported region/errors.
   */
  void fetchRegionalAlerts() {
    if (!enableAlertDisplay || WiFi.status() != WL_CONNECTED) {
      return;
    }

    String region = resolveAlertRegion();
    logPrint("Resolved alert region: ");
    logPrintln(region.length() > 0 ? region : "(none)");

    if (region == "US") {
      fetchNwsAlerts();
    } else if (region == "CA") {
      fetchEcccAlerts();
    } else if (region == "NO") {
      fetchMetNoAlerts();
    } else if (region == "DE") {
      fetchBrightSkyAlerts();
    } else {
      logPrintln("No regional alert provider configured for this location.");
    }
  }

  /**
   * Fetches upcoming Google Calendar events via a Google Apps Script Web App
   * URL (script.google.com). Non-fatal: logs and continues if not configured or
   * on error.
   */
  void fetchCalendarEvents() {
    calendarEventCount = 0;
    if (calendarUrl.length() == 0 || WiFi.status() != WL_CONNECTED) {
      return;
    }

    logPrint("Fetching Google Calendar events: ");
    logPrintln(calendarUrl);

    String calPayload =
        httpsGETRequest(calendarUrl.c_str(), "application/json");
    if (httpResponseCode != HTTP_CODE_OK) {
      logPrint("Calendar request failed with HTTP ");
      logPrintln(httpResponseCode);
      return;
    }

    JsonDocument calDoc;
    DeserializationError err = deserializeJson(calDoc, calPayload);
    if (err) {
      logPrint("Calendar JSON parse error: ");
      logPrintln(err.c_str());
      return;
    }

    JsonArray events = calDoc["events"].isNull()
                           ? calDoc.as<JsonArray>()
                           : calDoc["events"].as<JsonArray>();
    if (events.isNull()) {
      logPrintln("No events array in calendar response.");
      return;
    }

    for (JsonObject ev : events) {
      if (calendarEventCount >= MAX_CALENDAR_EVENTS) {
        break;
      }
      calendarEvents[calendarEventCount].date = ev["date"] | "";
      calendarEvents[calendarEventCount].time = ev["time"] | "";
      calendarEvents[calendarEventCount].title = ev["title"] | "(No title)";
      calendarEventCount++;
    }

    logPrint("Loaded calendar events count: ");
    logPrintln((int)calendarEventCount);
  }

  void setRTC(long unixTime) {
    if (unixTime == 0)
      return;

    struct timeval now = {.tv_sec = unixTime, .tv_usec = 0};
    settimeofday(&now, NULL);
  }

  /**
   * Processes the weather data from the Open-Meteo API response
   */
  bool processWeatherData() {
    if (weatherApiResponse["current"].isNull()) {
      return false;
    }

    int weatherCode = weatherApiResponse["current"]["weather_code"].as<int>();
    int isDay = weatherApiResponse["current"]["is_day"] | 1;
    long utcOffset = weatherApiResponse["utc_offset_seconds"].as<long>();

    // Extract and process weather data
    weatherInfo.weather = wmoCodeToWeatherMain(weatherCode);
    weatherInfo.icon = wmoCodeToIcon(weatherCode, isDay);
    weatherInfo.currentDateTime =
        weatherApiResponse["current"]["time"].as<long>() + utcOffset;
    long refreshLocalTime =
        parseHttpDateToLocalUnixTime(lastHttpDateHeader, utcOffset);
    weatherInfo.refreshDateTime =
        (refreshLocalTime > 0) ? refreshLocalTime : weatherInfo.currentDateTime;
    weatherInfo.sunrise =
        (!weatherApiResponse["daily"]["sunrise"].isNull() &&
         weatherApiResponse["daily"]["sunrise"].size() > 0)
            ? weatherApiResponse["daily"]["sunrise"][0].as<long>()
            : 0;
    weatherInfo.sunset =
        (!weatherApiResponse["daily"]["sunset"].isNull() &&
         weatherApiResponse["daily"]["sunset"].size() > 0)
            ? weatherApiResponse["daily"]["sunset"][0].as<long>()
            : 0;
    weatherInfo.temperature =
        String(weatherApiResponse["current"]["temperature_2m"].as<float>(), 1);
    weatherInfo.humidity =
        String(weatherApiResponse["current"]["relative_humidity_2m"].as<int>());
    weatherInfo.pressure = String((int)round(
        weatherApiResponse["current"]["surface_pressure"].as<float>()));
    weatherInfo.windSpeed =
        String(weatherApiResponse["current"]["wind_speed_10m"].as<float>(), 1);
    weatherInfo.timezone = weatherApiResponse["timezone"].as<String>();

    // Extract 5-day daily forecast summary (Today [0] through +4 days [4])
    JsonObject daily = weatherApiResponse["daily"];
    for (size_t d = 0; d < DAILY_FORECAST_DAYS; d++) {
      dailySummary[d].valid = false;
      if (!daily.isNull() && !daily["temperature_2m_max"].isNull() &&
          !daily["temperature_2m_min"].isNull() &&
          d < daily["temperature_2m_max"].size() &&
          d < daily["temperature_2m_min"].size()) {
        dailySummary[d].valid = true;
        dailySummary[d].dateTime =
            (!daily["time"].isNull() && d < daily["time"].size())
                ? daily["time"][d].as<long>() + utcOffset
                : 0;
        dailySummary[d].tempMax =
            (int)roundf(daily["temperature_2m_max"][d].as<float>());
        dailySummary[d].tempMin =
            (int)roundf(daily["temperature_2m_min"][d].as<float>());
        dailySummary[d].precipProb =
            (!daily["precipitation_probability_max"].isNull() &&
             d < daily["precipitation_probability_max"].size())
                ? daily["precipitation_probability_max"][d].as<int>()
                : 0;
        dailySummary[d].precipMm =
            (!daily["precipitation_sum"].isNull() &&
             d < daily["precipitation_sum"].size())
                ? daily["precipitation_sum"][d].as<float>()
                : 0.0f;
        int dailyCode = (!daily["weather_code"].isNull() &&
                         d < daily["weather_code"].size())
                            ? daily["weather_code"][d].as<int>()
                            : weatherCode;
        dailySummary[d].weatherCode = dailyCode;
        dailySummary[d].condition = wmoCodeToWeatherMain(dailyCode);
      }
    }

    // set RTC to current refresh time
    setRTC(weatherInfo.refreshDateTime);

    // Process temperature for display
    int decimalPos = weatherInfo.temperature.indexOf('.');
    weatherInfo.tempIntegerPart =
        String(weatherInfo.temperature.substring(0, decimalPos).toInt());
    weatherInfo.tempDecimalPart =
        String(weatherInfo.temperature.substring(decimalPos + 1).toInt());
    if (weatherInfo.tempDecimalPart.length() > 1) {
      weatherInfo.tempDecimalPart = weatherInfo.tempDecimalPart.substring(0, 1);
    }

    return true;
  }

  /**
   * Fetches weather information from Open-Meteo API (and NWS alerts if enabled)
   */
  bool getWeatherInfo(uint8_t maxRetries = 3) {
    uint8_t currentRetry = 0;
    bool success = false;

    while (currentRetry <= maxRetries && !success) {
      if (currentRetry > 0) {
        logPrint("Retry attempt ");
        logPrint(currentRetry);
        logPrintln(" of weather data fetch...");
        // Exponential backoff: 1s, 2s, 4s...
        delay(1000 * (1 << (currentRetry - 1)));
      }

      errorMessageBuffer = "getWeatherInfo() failed.";
      httpResponseCode = HTTP_NOT_REQUESTED_YET;
      if (WiFi.status() != WL_CONNECTED) {
        errorMessageBuffer = "Wireless network not available.";
        return false;
      }

      // Construct API endpoint URL and make request
      String apiEndPoint = constructApiEndpointUrl();
      jsonBuffer = httpsGETRequest(apiEndPoint.c_str());
      ledOn();

      // Check if connection error or timeout occurred
      if (httpResponseCode < 0) {
        logPrint("HTTP request failed with error code: ");
        logPrintln(httpResponseCode);
        currentRetry++;
        continue;
      }

      logPrint("HTTP response code: ");
      logPrintln(httpResponseCode);

      // Check for HTTP errors
      if (httpResponseCode != HTTP_CODE_OK) {
        errorMessageBuffer =
            "Weather API failed with HTTP code: " + String(httpResponseCode) +
            "\n\nThis typically happens due to a weather API server issue or "
            "invalid coordinates.";

        // Only retry on server errors (5xx) or certain client errors, don't
        // retry on 4xx errors
        if (httpResponseCode >= HTTP_CODE_INTERNAL_SERVER_ERROR ||
            httpResponseCode == HTTP_CODE_TOO_MANY_REQUESTS) {
          currentRetry++;
          continue;
        }
        return false; // Don't retry for other error codes
      }

      // Parse JSON response
      DeserializationError error =
          deserializeJson(weatherApiResponse, jsonBuffer);
      if (error) {
        logPrint(F("deserializeJson() failed: "));
        logPrintln(error.c_str());
        errorMessageBuffer = "JSON parsing error: " + String(error.c_str());
        currentRetry++;
        continue;
      }

      success = processWeatherData();
      if (!success) {
        currentRetry++;
        continue;
      }

      // Fetch regional weather alerts and Google Calendar events while WiFi is
      // active
      fetchRegionalAlerts();
      fetchCalendarEvents();

      if (lowPowerMode) {
        // Turn off wireless as soon as possible.
        wirelessOff();
      }
    }

    if (!success) {
      logPrintln("Failed to get weather info after all retry attempts");
    }
    return success;
  }

  /**
   * Finds the index in hourly.time corresponding to the current hour.
   */
  size_t findCurrentHourIndex() {
    JsonArray timeArray = weatherApiResponse["hourly"]["time"].as<JsonArray>();
    if (timeArray.isNull() || timeArray.size() == 0) {
      return 0;
    }
    long currentTimeUtc = weatherApiResponse["current"]["time"].as<long>();
    size_t idx = 0;
    for (size_t i = 0; i < timeArray.size(); i++) {
      if (timeArray[i].as<long>() <= currentTimeUtc) {
        idx = i;
      } else {
        break;
      }
    }
    return idx;
  }

  void drawDailyForecastItem(uint16_t x, uint16_t y, size_t dayIndex) {
    if (dayIndex >= DAILY_FORECAST_DAYS || !dailySummary[dayIndex].valid) {
      return;
    }

    char buffer[STRING_BUFFER_SIZE];
    const DailySummary &day = dailySummary[dayIndex];

    // Day label above icon ("Today", "Tomorrow", then weekday like "Sun")
    String dayLabel;
    if (dayIndex == 0) {
      dayLabel = "Today";
    } else if (dayIndex == 1) {
      dayLabel = "Tomorrow";
    } else if (day.dateTime > 0) {
      dayLabel = convertUnixTimeToSpecifiedDateTimeString(day.dateTime, "%a");
    } else {
      dayLabel = "Day " + String(dayIndex + 1);
    }
    memset(buffer, 0, sizeof(buffer));
    snprintf(buffer, sizeof(buffer), "%s", dayLabel.c_str());
    EPD_ShowStringCenterAligned(x + 50, y + 4, buffer, FONT_SIZE_16, BLACK);

    // Weather icon (daytime icon for daily summary)
    String iconKey = "icon_" + wmoCodeToIcon(day.weatherCode, 1) + "_sm";
    EPD_drawImage(x + 14, y + 8, getIcon(iconKey.c_str()));

    // High temperature (36px, left of vertical bar)
    memset(buffer, 0, sizeof(buffer));
    snprintf(buffer, sizeof(buffer), "%d", day.tempMax);
    EPD_ShowStringRightAligned(x + 52, y + 92, buffer, FONT_SIZE_36, BLACK);

    // Vertical separator bar
    EPD_DrawLine(x + 56, y + 78, x + 56, y + 106, BLACK);
    EPD_DrawLine(x + 57, y + 78, x + 57, y + 106, BLACK);

    // Low temperature (top-right of bar)
    memset(buffer, 0, sizeof(buffer));
    snprintf(buffer, sizeof(buffer), "%d", day.tempMin);
    EPD_ShowString(x + 62, y + 84, buffer, FONT_SIZE_16, BLACK, true);

    // Precipitation probability (bottom-right of bar)
    memset(buffer, 0, sizeof(buffer));
    snprintf(buffer, sizeof(buffer), "%d%%", day.precipProb);
    EPD_ShowString(x + 62, y + 100, buffer, FONT_SIZE_16, BLACK, true);
  }

  void drawDailyForecast(uint16_t baseX, uint16_t baseY, uint16_t length) {
    char buffer[STRING_BUFFER_SIZE];
    uint16_t daysToShow = min(length, (uint16_t)DAILY_FORECAST_DAYS);
    bool anyValid = false;
    for (uint16_t i = 0; i < daysToShow; i++) {
      if (dailySummary[i].valid) {
        anyValid = true;
        break;
      }
    }
    if (!anyValid) {
      memset(buffer, 0, sizeof(buffer));
      snprintf(buffer, sizeof(buffer), "%s",
               "Error: API not responding with daily forecast data.");
      EPD_ShowString(baseX, baseY + 60, buffer, FONT_SIZE_16, BLACK, true);
      return;
    }

    uint16_t x = baseX;
    for (uint16_t i = 0; i < daysToShow; i++) {
      drawDailyForecastItem(x, baseY, i);
      x += 104;
    }
  }

  void drawForecastItem(uint16_t x, uint16_t y, size_t hourIndex) {
    char buffer[STRING_BUFFER_SIZE];

    JsonObject hourly = weatherApiResponse["hourly"];
    String icon = "na_md";
    if (!hourly["weather_code"].isNull() &&
        hourIndex < hourly["weather_code"].size()) {
      int code = hourly["weather_code"][hourIndex].as<int>();
      int isDay =
          (!hourly["is_day"].isNull() && hourIndex < hourly["is_day"].size())
              ? hourly["is_day"][hourIndex].as<int>()
              : 1;
      icon = "icon_" + wmoCodeToIcon(code, isDay);
    }

    snprintf(buffer, sizeof(buffer), "%s_sm", icon.c_str());
    EPD_drawImage(x + 2, y, getIcon(buffer));

    // Time formatting ("10 AM" / "9 PM" in 12h, or "15" / ":00" in 24h)
    long forecastTimeInt = hourly["time"][hourIndex].as<long>() +
                           weatherApiResponse["utc_offset_seconds"].as<long>();
    String forecastTime = convertUnixTimeToShortDateTimeString(forecastTimeInt);

    int splitIndex = forecastTime.indexOf(' ');
    String topLine;
    String bottomLine;
    if (splitIndex != -1) {
      topLine = forecastTime.substring(0, splitIndex);
      bottomLine = forecastTime.substring(splitIndex + 1);
    } else {
      int colonIndex = forecastTime.indexOf(':');
      if (colonIndex != -1) {
        topLine = forecastTime.substring(0, colonIndex);
        bottomLine = forecastTime.substring(colonIndex);
      } else {
        topLine = forecastTime;
        bottomLine = "";
      }
    }

    memset(buffer, 0, sizeof(buffer));
    snprintf(buffer, sizeof(buffer), "%s", topLine.c_str());
    EPD_ShowStringRightAligned(x + 24, y + 80, buffer, FONT_SIZE_16, BLACK);

    if (bottomLine.length() > 0) {
      memset(buffer, 0, sizeof(buffer));
      snprintf(buffer, sizeof(buffer), "%s", bottomLine.c_str());
      EPD_ShowStringRightAligned(x + 28, y + 96, buffer, FONT_SIZE_16, BLACK);
    }

    // Vertical separator line
    uint16_t lineLength = 103;
    EPD_DrawLine(x + 35, y + 75, x + 35, y + lineLength, BLACK);
    EPD_DrawLine(x + 36, y + 75, x + 36, y + lineLength, BLACK);
    EPD_DrawLine(x + 37, y + 75, x + 37, y + lineLength, BLACK);

    // Temperature
    memset(buffer, 0, sizeof(buffer));
    float temp = hourly["temperature_2m"][hourIndex].as<float>();
    int tempInt = (int)temp;
    snprintf(buffer, sizeof(buffer), "%d", tempInt);
    EPD_ShowString(x + 46, y + 87, buffer, FONT_SIZE_38, BLACK, true);
  }

  void drawWeatherFutureForecast(uint16_t baseX, uint16_t baseY,
                                 uint16_t length) {
    uint16_t x = baseX;
    uint16_t y = baseY;
    char buffer[STRING_BUFFER_SIZE];

    // Check if hourly data exists
    if (weatherApiResponse["hourly"].isNull() ||
        weatherApiResponse["hourly"]["time"].isNull()) {
      memset(buffer, 0, sizeof(buffer));
      snprintf(buffer, sizeof(buffer), "%s",
               "Error: API not responding with hourly forecast data.");
      EPD_ShowString(x, y + 60, buffer, FONT_SIZE_16, BLACK, true);
      return;
    }

    // Get the hourly array length
    JsonArray timeArray = weatherApiResponse["hourly"]["time"].as<JsonArray>();
    JsonArray tempArray =
        weatherApiResponse["hourly"]["temperature_2m"].as<JsonArray>();
    size_t totalHours = timeArray.size();
    if (totalHours == 0) {
      memset(buffer, 0, sizeof(buffer));
      snprintf(buffer, sizeof(buffer), "%s",
               "Error: API responding with ZERO forecast data.");
      EPD_ShowString(x, y, buffer, FONT_SIZE_16, BLACK, true);
      return;
    }

    size_t startHourIdx = findCurrentHourIndex();
    size_t remainingHours =
        (totalHours > startHourIdx) ? (totalHours - startHourIdx) : 0;

    // Limit to either requested length or available data
    int forecastsToShow = min(length, (uint16_t)remainingHours);

    // Pre-calculate max temperature width to adjust starting position
    int maxTempWidth = 2; // Default for 2-digit temps like "25"
    for (uint16_t i = 1, counter = 0; counter < forecastsToShow;
         i += hourInterval, counter++) {
      size_t idx = startHourIdx + i;
      if (idx >= totalHours)
        break;
      if (!tempArray.isNull() && idx < tempArray.size()) {
        int tempInt = (int)tempArray[idx].as<float>();
        int tempWidth = 1;
        if (tempInt <= -10)
          tempWidth = 3; // -24, -10
        else if (tempInt < 0)
          tempWidth = 2; // -9 to -1
        else if (tempInt >= 100)
          tempWidth = 3; // 100+
        else if (tempInt >= 10)
          tempWidth = 2; // 10-99
        if (tempWidth > maxTempWidth)
          maxTempWidth = tempWidth;
      }
    }

    // Shift left if temperatures are 3+ characters wide
    if (maxTempWidth >= 3) {
      x -= 15 * (maxTempWidth - 2); // Shift 15px per extra character
    }

    for (uint16_t i = 1, counter = 0; counter < forecastsToShow;
         i += hourInterval, counter++) {
      size_t idx = startHourIdx + i;
      // Safely check if the hourly entry exists
      if (idx >= totalHours) {
        Serial.println("Index out of bounds for hourly forecast");
        break;
      }

      drawForecastItem(x, y, idx);

      // Offset for the next forecast item
      x = x + 107;
    }
  }

  void prepareGraphData(float *temps, float &minTemp, float &maxTemp,
                        int &hoursToShow, const size_t startHourIdx,
                        const size_t totalHours) {
    size_t remainingHours =
        (totalHours > startHourIdx) ? (totalHours - startHourIdx) : 0;
    // Determine how many hours to show based on available space
    hoursToShow = min((int)24, (int)remainingHours);

    minTemp = 100.0;
    maxTemp = -100.0;

    JsonArray tempArray =
        weatherApiResponse["hourly"]["temperature_2m"].as<JsonArray>();

    // Find min and max temperatures for scaling
    for (int i = 0; i < hoursToShow; i++) {
      size_t idx = startHourIdx + i;
      if (idx >= totalHours)
        break;

      temps[i] = tempArray[idx].as<float>();

      if (temps[i] < minTemp)
        minTemp = temps[i];
      if (temps[i] > maxTemp)
        maxTemp = temps[i];
    }

    // Add padding to min/max for visual appeal
    float tempRange = maxTemp - minTemp;
    if (tempRange < 5.0) {
      // If range is too small, expand it
      float padding = (5.0 - tempRange) / 2;
      minTemp -= padding;
      maxTemp += padding;
    } else {
      // Add 10% padding on top and bottom
      minTemp -= tempRange * 0.1;
      maxTemp += tempRange * 0.1;
    }
  }

  void drawGraph(uint16_t baseX, uint16_t baseY, uint16_t graphWidth,
                 uint16_t graphHeight, uint8_t lineWidth = 1) {
    uint16_t x = baseX;
    uint16_t y = baseY;
    char buffer[STRING_BUFFER_SIZE];

    // Check if hourly data exists
    if (weatherApiResponse["hourly"].isNull() ||
        weatherApiResponse["hourly"]["time"].isNull()) {
      memset(buffer, 0, sizeof(buffer));
      snprintf(buffer, sizeof(buffer), "%s", "Error: No hourly data available");
      EPD_ShowString(x, y, buffer, FONT_SIZE_16, BLACK, true);
      return;
    }

    // Get the hourly array length
    JsonArray timeArray = weatherApiResponse["hourly"]["time"].as<JsonArray>();
    size_t totalHours = timeArray.size();
    size_t startHourIdx = findCurrentHourIndex();
    if (totalHours <= startHourIdx + 1) {
      memset(buffer, 0, sizeof(buffer));
      snprintf(buffer, sizeof(buffer), "%s",
               "Error: Insufficient forecast data");
      EPD_ShowString(x, y, buffer, FONT_SIZE_16, BLACK, true);
      return;
    }

    // Prepare data for graph
    float temps[24]; // Maximum 24 hours
    float minTemp, maxTemp;
    int hoursToShow;

    prepareGraphData(temps, minTemp, maxTemp, hoursToShow, startHourIdx,
                     totalHours);

    // Calculate scaling factor and point spacing
    float tempRange = maxTemp - minTemp;
    float scale = graphHeight / tempRange;
    uint16_t pointSpacing = graphWidth / (hoursToShow - 1);

    // Draw temperature points and connecting lines
    for (int i = 1; i < hoursToShow; i++) {
      // Calculate positions
      uint16_t x1 = x + (i - 1) * pointSpacing;
      uint16_t y1 =
          y + graphHeight - (uint16_t)((temps[i - 1] - minTemp) * scale);
      uint16_t x2 = x + i * pointSpacing;
      uint16_t y2 = y + graphHeight - (uint16_t)((temps[i] - minTemp) * scale);

      // Draw thicker line by drawing multiple lines if lineWidth > 1
      for (int w = 0; w < lineWidth; w++) {
        EPD_DrawLine(x1, y1 + w, x2, y2 + w, BLACK);
      }

      // Draw dots at data points
      EPD_DrawCircle(x1, y1, 2, BLACK, true);
      if (i == hoursToShow - 1) {
        EPD_DrawCircle(x2, y2, 2, BLACK, true);
      }

      // Label every 6 hours with time
      if (i % 6 == 0 || i == 1) {
        long time = timeArray[startHourIdx + i].as<long>() +
                    weatherApiResponse["utc_offset_seconds"].as<long>();
        String timeStr = convertUnixTimeToShortDateTimeString(time);

        memset(buffer, 0, sizeof(buffer));
        snprintf(buffer, sizeof(buffer), "%s", timeStr.c_str());
        EPD_ShowString(x2 - 10, y + graphHeight + 5, buffer, FONT_SIZE_8, BLACK,
                       true);
      }
    }

    // Draw temperature scale on right side
    memset(buffer, 0, sizeof(buffer));
    snprintf(buffer, sizeof(buffer), "%.1f°", maxTemp);
    EPD_ShowString(x + graphWidth + 5, y, buffer, FONT_SIZE_8, BLACK, true);

    memset(buffer, 0, sizeof(buffer));
    snprintf(buffer, sizeof(buffer), "%.1f°", minTemp);
    EPD_ShowString(x + graphWidth + 5, y + graphHeight - 10, buffer,
                   FONT_SIZE_8, BLACK, true);
  }

  void displayCurrentInfo(uint16_t baseX, uint16_t baseY) {
    uint16_t centerX = baseX;
    uint16_t y = baseY;
    uint16_t unitOffsetX = 4;
    uint16_t unitOffsetY = 5;

    char buffer[STRING_BUFFER_SIZE];

    // Pressure
    memset(buffer, 0, sizeof(buffer));
    snprintf(buffer, sizeof(buffer), "%s", weatherInfo.pressure.c_str());
    EPD_ShowStringRightAligned(centerX, y, buffer, FONT_SIZE_36, BLACK);

    memset(buffer, 0, sizeof(buffer));
    snprintf(buffer, sizeof(buffer), "hPa");
    EPD_ShowString(centerX + unitOffsetX, y + unitOffsetY, buffer, FONT_SIZE_16,
                   BLACK, false);

    // Humidity
    y += 40;
    memset(buffer, 0, sizeof(buffer));
    snprintf(buffer, sizeof(buffer), "%s", weatherInfo.humidity.c_str());
    EPD_ShowStringRightAligned(centerX, y, buffer, FONT_SIZE_36, BLACK);

    memset(buffer, 0, sizeof(buffer));
    snprintf(buffer, sizeof(buffer), "%%RH");
    EPD_ShowString(centerX + unitOffsetX, y + unitOffsetY, buffer, FONT_SIZE_16,
                   BLACK, false);

    // Alert, Snow, rain, UVI, or wind speed
    y += 40;
    displayAdditionalInfoLine(centerX, y, unitOffsetX, unitOffsetY);
  }

  /**
   * truncate the string to a maximum length, appending "..." if truncated
   * if the maxLength is set to 10, the string will be truncated to 10
   * characters with "..." appended
   * @param str The string to truncate
   * @param maxLength The maximum length of the string
   * @return The truncated string
   */
  String truncateString(const String &str, size_t maxLength) {
    if (str.length() > maxLength) {
      return str.substring(0, maxLength) + "...";
    }
    return str;
  }

  /**
   * Displays weather alerts information
   * @param baseX X position for alert display
   * @param baseY Y position for alert display
   * @return true if alerts were displayed, false otherwise
   */
  bool displayAlerts(uint16_t baseX, uint16_t baseY) {
    uint16_t x = baseX;
    uint16_t y = baseY;
    char buffer[STRING_BUFFER_SIZE];

    // Get the first/most recent alert
    JsonObject alert = weatherApiResponse["alerts"][0];

    // Display alert icon
    EPD_drawImage(x, y + 5, getIcon("warning_sm", "na_md"));

    y += 30;
    x += 75;

    // Display event type
    if (!alert["event"].isNull()) {
      String event = alert["event"].as<String>();
      event = truncateString(event, 10);

      event[0] = toupper(event[0]);
      memset(buffer, 0, sizeof(buffer));
      snprintf(buffer, sizeof(buffer), "%s", event.c_str());
      EPD_ShowString(x - 10, y - 5, buffer, FONT_SIZE_36, BLACK, true);
      y += 45;
    }

    if (!alert["sender_name"].isNull() && !alert["start"].isNull() &&
        !alert["end"].isNull()) {
      String senderName = alert["sender_name"].as<String>();
      senderName = truncateString(senderName, 28);
      long startTime = alert["start"].as<long>();
      long endTime = alert["end"].as<long>();

      String startTimeStr = convertUnixTimeToShortDateTimeString(startTime);
      String startStr =
          convertUnixTimeToSpecifiedDateTimeString(startTime, "%a ") +
          startTimeStr;
      String endTimeStr = convertUnixTimeToShortDateTimeString(endTime);
      String endStr =
          convertUnixTimeToSpecifiedDateTimeString(endTime, "%a ") + endTimeStr;

      // check if startTime and EndTime are the same day, we don't need to put
      // %a twice
      if (startStr.substring(0, 3) == endStr.substring(0, 3)) {
        endStr = endStr.substring(4);
      }

      memset(buffer, 0, sizeof(buffer));
      snprintf(buffer, sizeof(buffer), "%s to %s %s", startStr.c_str(),
               endStr.c_str(), senderName.c_str());
      EPD_ShowString(x - 60, y, buffer, FONT_SIZE_16, BLACK, true);
      y += 25;
    }

    if (!alert["description"].isNull()) {
      String desc = removeWarningDescription(alert["description"].as<String>());

      int maxLength = 200; // Maximum length for description
      desc = truncateString(desc, maxLength);

      memset(buffer, 0, sizeof(buffer));
      snprintf(buffer, sizeof(buffer), "%s", desc.c_str());
      EPD_ShowString(x - 60, y, buffer, FONT_SIZE_16, BLACK, false);
    }

    return true;
  }

  String removeWarningDescription(String description) {
    // Remove boilerplate headings and line breaks from alert descriptions
    const char *toBeRemoved[] = {
        "* WHAT...",
        "* WHERE...",
        "* WHEN...",
        "* IMPACTS...",
        "* ADDITIONAL DETAILS...",
        "What:\n",
        "\n\nHazard:\n",
        "\n\nTiming:\n",
        "\n\nAdditional information:\n",
        "\n\n###\n",
        "\n" // replace all line breaks
    };
    const int toBeRemovedCount = sizeof(toBeRemoved) / sizeof(toBeRemoved[0]);
    for (int i = 0; i < toBeRemovedCount; ++i) {
      int idx;
      while ((idx = description.indexOf(toBeRemoved[i])) != -1) {
        if (strcmp(toBeRemoved[i], "\n") == 0) {
          description.setCharAt(idx, ' ');
        } else {
          description.remove(idx, strlen(toBeRemoved[i]));
        }
      }
    }

    return description;
  }

  void displayAdditionalInfoLine(uint16_t centerX, uint16_t y,
                                 uint16_t unitOffsetX, uint16_t unitOffsetY) {
    char buffer[STRING_BUFFER_SIZE];

    float snowfallCm = weatherApiResponse["current"]["snowfall"] | 0.0f;
    float rainMm = weatherApiResponse["current"]["rain"] | 0.0f;
    float uvi = weatherApiResponse["current"]["uv_index"] | 0.0f;

    if (snowfallCm > 0.0f) {
      // Snow (Open-Meteo returns snowfall in cm; convert to mm for display)
      memset(buffer, 0, sizeof(buffer));
      snprintf(buffer, sizeof(buffer), "%.1f", snowfallCm * 10.0f);
      EPD_ShowStringRightAligned(centerX, y, buffer, FONT_SIZE_36, BLACK);

      memset(buffer, 0, sizeof(buffer));
      snprintf(buffer, sizeof(buffer), "mm snow");
      EPD_ShowString(centerX + unitOffsetX, y + unitOffsetY, buffer,
                     FONT_SIZE_16, BLACK, false);
    } else if (rainMm > 0.0f) {
      // Rain
      memset(buffer, 0, sizeof(buffer));
      snprintf(buffer, sizeof(buffer), "%.1f", rainMm);
      EPD_ShowStringRightAligned(centerX, y, buffer, FONT_SIZE_36, BLACK);

      memset(buffer, 0, sizeof(buffer));
      snprintf(buffer, sizeof(buffer), "mm rain");
      EPD_ShowString(centerX + unitOffsetX, y + unitOffsetY, buffer,
                     FONT_SIZE_16, BLACK, false);
    } else if (uvi > UVI_DISPLAY_THRESHOLD) {
      // UVI
      memset(buffer, 0, sizeof(buffer));
      snprintf(buffer, sizeof(buffer), "%.1f", uvi);
      EPD_ShowStringRightAligned(centerX, y, buffer, FONT_SIZE_36, BLACK);

      memset(buffer, 0, sizeof(buffer));
      snprintf(buffer, sizeof(buffer), "UVI");
      EPD_ShowString(centerX + unitOffsetX, y + unitOffsetY, buffer,
                     FONT_SIZE_16, BLACK, false);
    } else if (weatherInfo.windSpeed.toFloat() > WIND_SPEED_DISPLAY_THRESHOLD) {
      // Wind speed
      memset(buffer, 0, sizeof(buffer));
      snprintf(buffer, sizeof(buffer), "%.1f",
               weatherApiResponse["current"]["wind_speed_10m"].as<float>());
      EPD_ShowStringRightAligned(centerX, y, buffer, FONT_SIZE_36, BLACK);

      memset(buffer, 0, sizeof(buffer));
      snprintf(buffer, sizeof(buffer), "m/s wind");
      EPD_ShowString(centerX + unitOffsetX, y + unitOffsetY, buffer,
                     FONT_SIZE_16, BLACK, false);
    }
  }

  void displayTemperature(uint16_t x, uint16_t y, bool isLarge) {
    char buffer[STRING_BUFFER_SIZE];
    memset(buffer, 0, sizeof(buffer));
    if (isLarge) {
      snprintf(buffer, sizeof(buffer), "%s",
               weatherInfo.tempIntegerPart.c_str());
      EPD_ShowStringRightAligned(x, y, buffer, FONT_SIZE_92, BLACK);
      memset(buffer, 0, sizeof(buffer));
      EPD_drawImage(x, y, degrees_sm);
      snprintf(buffer, sizeof(buffer), (units == "metric") ? "C" : "F");
      EPD_ShowString(x + 10, y + 24, buffer, FONT_SIZE_36, BLACK);
    } else {
      EPD_drawImage(x - 2, y - 20, degrees_sm);
      snprintf(buffer, sizeof(buffer), (units == "metric") ? "C" : "F");
      EPD_ShowString(x + 12, y, buffer, FONT_SIZE_36, BLACK);

      snprintf(buffer, sizeof(buffer), "%s",
               weatherInfo.tempIntegerPart.c_str());
      EPD_ShowStringRightAligned(x, y, buffer, FONT_SIZE_36, BLACK);
    }
  }

  /**
   * Formats a daily precipitation amount in mm (metric) or inches (imperial).
   * Returns empty string when precipitation is negligible (< 0.1 mm).
   */
  String formatDailyPrecipAmount(float precipMm) {
    if (precipMm < 0.1f) {
      return "";
    }
    char buf[24];
    if (units == "imperial") {
      float inches = precipMm / 25.4f;
      if (inches < 0.01f) {
        return "";
      }
      snprintf(buf, sizeof(buf), "%.2fin", inches);
    } else {
      snprintf(buf, sizeof(buf), "%.1fmm", precipMm);
    }
    return String(buf);
  }

  /**
   * Renders Today & Tomorrow High/Low/Precipitation summary in the top-center
   * area (x=278..558, y=10..148) when no calendar URL and no alert are active.
   */
  void displayDailySummaryCompact(uint16_t baseX, uint16_t baseY) {
    char buffer[STRING_BUFFER_SIZE];
    const char *dayLabels[2] = {"TODAY", "TOMORROW"};
    const uint16_t rightEdge = baseX + 278;

    for (size_t d = 0; d < 2; d++) {
      if (!dailySummary[d].valid) {
        continue;
      }

      uint16_t rowTop = baseY + d * 74;
      String precipAmt = formatDailyPrecipAmount(dailySummary[d].precipMm);

      // Day header + condition (e.g., "TODAY - Clouds")
      String header = String(dayLabels[d]);
      if (dailySummary[d].condition.length() > 0) {
        header += " - " + dailySummary[d].condition;
      }
      header = truncateString(header, precipAmt.length() > 0 ? 18 : 24);
      memset(buffer, 0, sizeof(buffer));
      snprintf(buffer, sizeof(buffer), "%s", header.c_str());
      EPD_ShowString(baseX, rowTop + 8, buffer, FONT_SIZE_16, BLACK, true);

      // Optional precipitation volume right-aligned on header row
      if (precipAmt.length() > 0) {
        memset(buffer, 0, sizeof(buffer));
        snprintf(buffer, sizeof(buffer), "%s", precipAmt.c_str());
        EPD_ShowStringRightAligned(rightEdge, rowTop + 8, buffer, FONT_SIZE_16,
                                   BLACK);
      }

      // High / Low temperature in 36px font
      memset(buffer, 0, sizeof(buffer));
      snprintf(buffer, sizeof(buffer), "%d/%d", dailySummary[d].tempMax,
               dailySummary[d].tempMin);
      EPD_ShowString(baseX, rowTop + 39, buffer, FONT_SIZE_36, BLACK, true);

      // Precipitation probability with raindrop icon
      EPD_drawImage(baseX + 152, rowTop + 23, getIcon("raindrop_sm"));
      memset(buffer, 0, sizeof(buffer));
      snprintf(buffer, sizeof(buffer), "%d%%", dailySummary[d].precipProb);
      EPD_ShowString(baseX + 184, rowTop + 39, buffer, FONT_SIZE_36, BLACK,
                     true);

      // Divider between Today and Tomorrow
      if (d == 0) {
        EPD_DrawLine(baseX, rowTop + 66, rightEdge, rowTop + 66, BLACK);
      }
    }
  }

  /**
   * Truncates a string so its rendered width in fontSize does not exceed
   * maxWidth pixels, appending "..." if truncated.
   */
  String truncateToPixelWidth(const String &str, FontSize fontSize,
                              uint16_t maxWidth) {
    if (EPD_GetStringWidth(str.c_str(), fontSize) <= maxWidth) {
      return str;
    }
    const String ellipsis = "...";
    uint16_t ellipsisWidth = EPD_GetStringWidth(ellipsis.c_str(), fontSize);
    if (maxWidth <= ellipsisWidth) {
      return ellipsis;
    }
    String result = str;
    while (result.length() > 0 &&
           EPD_GetStringWidth((result + ellipsis).c_str(), fontSize) >
               maxWidth) {
      result.remove(result.length() - 1);
    }
    result.trim();
    return result + ellipsis;
  }

  /**
   * Maps a WMO weather code to a human-readable precipitation type word.
   */
  String precipTypeFromWmoCode(int code, bool capitalized) {
    const char *word = "rain";
    switch (code) {
    case 51:
    case 53:
    case 55:
    case 56:
    case 57:
      word = "drizzle";
      break;
    case 66:
    case 67:
      word = "freezing rain";
      break;
    case 71:
    case 73:
    case 75:
    case 77:
    case 85:
    case 86:
      word = "snow";
      break;
    case 95:
    case 96:
    case 99:
      word = "storms";
      break;
    default:
      word = "rain";
      break;
    }
    String s(word);
    if (capitalized && s.length() > 0) {
      s[0] = toupper(s[0]);
    }
    return s;
  }

  bool isPrecipWmoCode(int code) {
    return (code >= 51 && code <= 86) || (code >= 95 && code <= 99);
  }

  /**
   * Builds a concise plain-English precipitation summary using current
   * conditions, next-24h hourly precipitation probabilities, and 5-day daily
   * outlook.
   */
  String buildPrecipitationMessage() {
    long utcOffset = weatherApiResponse["utc_offset_seconds"].as<long>();
    int currentCode = weatherApiResponse["current"]["weather_code"] | 0;
    float currentRain = weatherApiResponse["current"]["rain"] | 0.0f;
    float currentSnow = weatherApiResponse["current"]["snowfall"] | 0.0f;
    bool rainingNow = (currentRain > 0.0f) || (currentSnow > 0.0f) ||
                      isPrecipWmoCode(currentCode);

    JsonArray hTime = weatherApiResponse["hourly"]["time"].as<JsonArray>();
    JsonArray hProb = weatherApiResponse["hourly"]["precipitation_probability"]
                          .as<JsonArray>();
    JsonArray hCode =
        weatherApiResponse["hourly"]["weather_code"].as<JsonArray>();
    size_t totalHours = (!hTime.isNull() && !hProb.isNull())
                            ? min(hTime.size(), hProb.size())
                            : 0;
    size_t startIdx = (totalHours > 0) ? findCurrentHourIndex() : 0;

    // 1. Currently precipitating
    if (rainingNow) {
      int activeCode = (currentSnow > 0.0f) ? 73 : currentCode;
      String pTypeCap = precipTypeFromWmoCode(activeCode, true);
      if (pTypeCap == "Snow")
        pTypeCap = "Snowing";
      else if (pTypeCap == "Rain")
        pTypeCap = "Raining";
      else if (pTypeCap == "Drizzle")
        pTypeCap = "Drizzling";

      for (size_t i = startIdx + 1; i < totalHours && i <= startIdx + 18; i++) {
        int prob = hProb[i].as<int>();
        int code =
            (!hCode.isNull() && i < hCode.size()) ? hCode[i].as<int>() : 0;
        if (prob < 30 && !isPrecipWmoCode(code)) {
          String endTime = convertUnixTimeToShortDateTimeString(
              hTime[i].as<long>() + utcOffset);
          return pTypeCap + " now, easing around " + endTime;
        }
      }
      return pTypeCap + " continuing into tonight";
    }

    // 2. Scan next 24 hours for upcoming precipitation (>= 30% chance)
    size_t firstPrecipIdx = 0;
    bool foundPrecip = false;
    int peakProb = 0;
    int peakCode = 61;
    for (size_t i = startIdx + 1; i < totalHours && i <= startIdx + 24; i++) {
      int prob = hProb[i].as<int>();
      if (prob >= 30) {
        if (!foundPrecip) {
          foundPrecip = true;
          firstPrecipIdx = i;
        }
        if (prob > peakProb) {
          peakProb = prob;
          if (!hCode.isNull() && i < hCode.size() &&
              isPrecipWmoCode(hCode[i].as<int>())) {
            peakCode = hCode[i].as<int>();
          }
        }
      }
    }

    if (foundPrecip) {
      String pTypeCap = precipTypeFromWmoCode(peakCode, true);
      String pTypeLow = precipTypeFromWmoCode(peakCode, false);
      String whenStr;
      if (firstPrecipIdx <= startIdx + 1) {
        whenStr = "soon";
      } else {
        String startStr = convertUnixTimeToShortDateTimeString(
            hTime[firstPrecipIdx].as<long>() + utcOffset);
        whenStr = "around " + startStr;
      }

      if (peakProb >= 70) {
        return pTypeCap + " likely " + whenStr + " (" + String(peakProb) + "%)";
      } else if (peakProb >= 45) {
        return pTypeCap + " possible " + whenStr + " (" + String(peakProb) +
               "%)";
      } else {
        return "Slight chance of " + pTypeLow + " " + whenStr + " (" +
               String(peakProb) + "%)";
      }
    }

    // 3. Dry next 24h; check Tomorrow
    if (dailySummary[1].valid && dailySummary[1].precipProb >= 30) {
      String pTypeLow =
          precipTypeFromWmoCode(dailySummary[1].weatherCode, false);
      String likelihood =
          (dailySummary[1].precipProb >= 65) ? " likely " : " possible ";
      return "Dry today; " + pTypeLow + likelihood + "tmrw (" +
             String(dailySummary[1].precipProb) + "%)";
    }

    // 4. Dry through tomorrow; check days 2-4
    for (size_t d = 2; d < DAILY_FORECAST_DAYS; d++) {
      if (dailySummary[d].valid && dailySummary[d].precipProb >= 40) {
        String pTypeLow =
            precipTypeFromWmoCode(dailySummary[d].weatherCode, false);
        String dayName = (dailySummary[d].dateTime > 0)
                             ? convertUnixTimeToSpecifiedDateTimeString(
                                   dailySummary[d].dateTime, "%a")
                             : ("Day " + String(d + 1));
        return "Dry through tmrw; " + pTypeLow + " " + dayName + " (" +
               String(dailySummary[d].precipProb) + "%)";
      }
    }

    return "No precipitation expected";
  }

  /**
   * Renders the plain-English precipitation message in the bottom-left box
   * (up to 2 word-wrapped lines in FONT_SIZE_16).
   */
  void displayPrecipitationMessage(uint16_t baseX, uint16_t baseY,
                                   uint16_t maxWidth, bool drawTopDivider) {
    String msg = buildPrecipitationMessage();
    if (msg.length() == 0) {
      return;
    }

    if (drawTopDivider) {
      EPD_DrawLine(baseX, baseY - 10, baseX + maxWidth, baseY - 10, BLACK);
    }

    char buffer[STRING_BUFFER_SIZE];
    if (EPD_GetStringWidth(msg.c_str(), FONT_SIZE_16) <= maxWidth) {
      memset(buffer, 0, sizeof(buffer));
      snprintf(buffer, sizeof(buffer), "%s", msg.c_str());
      EPD_ShowString(baseX, baseY + 7, buffer, FONT_SIZE_16, BLACK, true);
      return;
    }

    // Word-wrap into 2 lines within maxWidth
    int splitPos = -1;
    for (int i = 0; i < (int)msg.length(); i++) {
      if (msg.charAt(i) == ' ') {
        String candidate = msg.substring(0, i);
        if (EPD_GetStringWidth(candidate.c_str(), FONT_SIZE_16) <= maxWidth) {
          splitPos = i;
        } else {
          break;
        }
      }
    }

    String line1;
    String line2;
    if (splitPos > 0) {
      line1 = msg.substring(0, splitPos);
      line2 = msg.substring(splitPos + 1);
      line2.trim();
      line2 = truncateToPixelWidth(line2, FONT_SIZE_16, maxWidth);
    } else {
      line1 = truncateToPixelWidth(msg, FONT_SIZE_16, maxWidth);
      line2 = "";
    }

    memset(buffer, 0, sizeof(buffer));
    snprintf(buffer, sizeof(buffer), "%s", line1.c_str());
    EPD_ShowString(baseX, baseY, buffer, FONT_SIZE_16, BLACK, true);

    if (line2.length() > 0) {
      memset(buffer, 0, sizeof(buffer));
      snprintf(buffer, sizeof(buffer), "%s", line2.c_str());
      EPD_ShowString(baseX, baseY + 15, buffer, FONT_SIZE_16, BLACK, true);
    }
  }

  /**
   * Renders Today & Tomorrow High/Low/Precipitation summary stacked in the
   * left column (x=16..252, y=12..226) when an alert occupies the top-center
   * area and no calendar URL is configured.
   */
  void displayDailySummaryColumn(uint16_t baseX, uint16_t baseY) {
    char buffer[STRING_BUFFER_SIZE];
    const char *dayLabels[2] = {"TODAY", "TOMORROW"};

    for (size_t d = 0; d < 2; d++) {
      if (!dailySummary[d].valid) {
        continue;
      }

      uint16_t rowTop = baseY + d * 108;

      String header = String(dayLabels[d]);
      if (dailySummary[d].condition.length() > 0) {
        header += " - " + dailySummary[d].condition;
      }
      header = truncateToPixelWidth(header, FONT_SIZE_16, 236);
      memset(buffer, 0, sizeof(buffer));
      snprintf(buffer, sizeof(buffer), "%s", header.c_str());
      EPD_ShowString(baseX, rowTop + 6, buffer, FONT_SIZE_16, BLACK, true);

      memset(buffer, 0, sizeof(buffer));
      snprintf(buffer, sizeof(buffer), "%d / %d", dailySummary[d].tempMax,
               dailySummary[d].tempMin);
      EPD_ShowString(baseX, rowTop + 38, buffer, FONT_SIZE_36, BLACK, true);

      String precipAmt = formatDailyPrecipAmount(dailySummary[d].precipMm);
      memset(buffer, 0, sizeof(buffer));
      if (precipAmt.length() > 0) {
        snprintf(buffer, sizeof(buffer), "Precip: %d%% (%s)",
                 dailySummary[d].precipProb, precipAmt.c_str());
      } else {
        snprintf(buffer, sizeof(buffer), "Precip: %d%%",
                 dailySummary[d].precipProb);
      }
      EPD_ShowString(baseX, rowTop + 72, buffer, FONT_SIZE_16, BLACK, true);

      if (d == 0) {
        EPD_DrawLine(baseX, rowTop + 94, baseX + 234, rowTop + 94, BLACK);
      }
    }
  }

  void displayCalendarEvents(uint16_t baseX, uint16_t baseY, bool alertActive) {
    if (calendarUrl.length() == 0) {
      return;
    }

    char buffer[STRING_BUFFER_SIZE];
    uint16_t x = baseX;
    uint16_t y = baseY;

    if (calendarEventCount == 0) {
      snprintf(buffer, sizeof(buffer), "No upcoming events");
      EPD_ShowString(x, y + 20, buffer,
                     alertActive ? FONT_SIZE_16 : FONT_SIZE_36, BLACK, true);
      return;
    }

    // Fewer entries (up to 4) rendered with larger FONT_SIZE_36 event titles
    // and compact FONT_SIZE_16 date/time sub-headers.
    const size_t maxDisplayEvents = 4;
    const uint16_t rowStep = 56;
    size_t count = min(calendarEventCount, maxDisplayEvents);

    for (size_t i = 0; i < count; i++) {
      uint16_t titleY = y + 24;
      if (titleY + 14 > 232) {
        break;
      }

      // Top rows that finish above the 5-day strip (y=152) can span across
      // x=16..552 when no alert is active; lower rows stay in left column.
      uint16_t maxWidth = (!alertActive && (titleY + 14) < 152) ? 536 : 242;

      String meta = "";
      if (calendarEvents[i].date.length() > 0) {
        meta += calendarEvents[i].date;
      }
      if (calendarEvents[i].time.length() > 0) {
        if (meta.length() > 0) {
          meta += " ";
        }
        meta += calendarEvents[i].time;
      }
      if (meta.length() > 0) {
        meta = truncateToPixelWidth(meta, FONT_SIZE_16, maxWidth);
        memset(buffer, 0, sizeof(buffer));
        snprintf(buffer, sizeof(buffer), "%s", meta.c_str());
        EPD_ShowString(x, y, buffer, FONT_SIZE_16, BLACK, true);
      }

      String title =
          truncateToPixelWidth(calendarEvents[i].title, FONT_SIZE_36, maxWidth);
      memset(buffer, 0, sizeof(buffer));
      snprintf(buffer, sizeof(buffer), "%s", title.c_str());
      EPD_ShowString(x, titleY, buffer, FONT_SIZE_36, BLACK, true);

      y += rowStep;
    }
  }

  void displayWeatherForecast() {
    clearScreen();
    char buffer[STRING_BUFFER_SIZE];

    long displayTime = (weatherInfo.refreshDateTime > 0)
                           ? weatherInfo.refreshDateTime
                           : weatherInfo.currentDateTime;

    // Top-right date
    memset(buffer, 0, sizeof(buffer));
    snprintf(buffer, sizeof(buffer), "%s ",
             convertUnixTimeToDisplayFormat(displayTime).c_str());
    EPD_ShowStringRightAligned(790, 20, buffer, FONT_SIZE_36, BLACK);

    // Refresh time (12H AM/PM or 24H HH:MM based on config)
    String timeStr = convertUnixTimeToTimeString(displayTime);

    bool alertActive = enableAlertDisplay &&
                       !weatherApiResponse["alerts"].isNull() &&
                       weatherApiResponse["alerts"].size() > 0;
    bool hasCalendar = calendarUrl.length() > 0;

    if (alertActive) {
      memset(buffer, 0, sizeof(buffer));
      snprintf(buffer, sizeof(buffer), "Updated %s", timeStr.c_str());
      EPD_ShowStringRightAligned(785, 48, buffer, FONT_SIZE_16, BLACK);

      displayAlerts(270, 0);
      displayTemperature(740, 76, false);
    } else {
      memset(buffer, 0, sizeof(buffer));
      snprintf(buffer, sizeof(buffer), "Updated %s", timeStr.c_str());
      EPD_ShowStringRightAligned(785, 50, buffer, FONT_SIZE_16, BLACK);

      displayTemperature(740, 105, true);
    }

    if (hasCalendar) {
      // Display fewer, larger Google Calendar events in the left/middle area
      displayCalendarEvents(16, 14, alertActive);
      displayPrecipitationMessage(16, 248, 242, true);
    } else if (alertActive) {
      // No calendar + alert active: show Today/Tomorrow summary in left column
      displayDailySummaryColumn(16, 12);
      displayPrecipitationMessage(16, 248, 242, true);
    } else {
      // Default layout (no calendar, no alert): large weather icon on left,
      // Today/Tomorrow high-low-precipitation summary in the center, and
      // plain-English precipitation message under the icon
      String iconName = "icon_" + weatherInfo.icon + "_lg";
      EPD_drawImage(0, 0, getIcon(iconName.c_str()));
      displayDailySummaryCompact(278, 10);
      displayPrecipitationMessage(12, 248, 250, false);
    }

    // Display 5-day forecast (Today, Tomorrow, and next 3 days)
    drawDailyForecast(270, 158, DAILY_FORECAST_DAYS);

    // clock frequency is reduced to 80MHz to save power
    if (lowPowerMode) {
      setCpuFrequencyMhz(80);
    }

    ledOff();
    // Update display
    EPD_Display(imageBW);
    EPD_Update();
    EPD_DeepSleep();
  }

  void displayError() {
    char title[] = "Something went wrong.";
    char description[STRING_BUFFER_SIZE];
    memset(description, 0, sizeof(description));
    strncpy(description, errorMessageBuffer.c_str(), sizeof(description) - 1);
    displayErrorMessage(title, description);
  }

  void displayTypographyTest() {
    clearScreen();
    char buffer[STRING_BUFFER_SIZE];

    memset(buffer, 0, sizeof(buffer));
    snprintf(
        buffer, sizeof(buffer),
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz 0123456789");
    EPD_ShowStringRightAligned(790, 10, buffer, FONT_SIZE_8, BLACK);

    memset(buffer, 0, sizeof(buffer));
    snprintf(
        buffer, sizeof(buffer),
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz 0123456789");
    EPD_ShowStringRightAligned(790, 30, buffer, FONT_SIZE_16, BLACK);

    memset(buffer, 0, sizeof(buffer));
    snprintf(buffer, sizeof(buffer), "ABCDEFGHIJKLMNOPQRSTUVWXYZ");
    EPD_ShowStringRightAligned(790, 80, buffer, FONT_SIZE_36, BLACK);

    memset(buffer, 0, sizeof(buffer));
    snprintf(buffer, sizeof(buffer), "abcdefghijklmnopqrstuvwxyz");
    EPD_ShowStringRightAligned(790, 120, buffer, FONT_SIZE_36, BLACK);

    memset(buffer, 0, sizeof(buffer));
    snprintf(buffer, sizeof(buffer), "Pixel perfection");
    EPD_ShowStringRightAligned(790, 210, buffer, FONT_SIZE_92, BLACK);

    // Update display
    EPD_Display(imageBW);
    EPD_PartUpdate();
    EPD_DeepSleep();
  }

  void displayCustomFontTest() {
    clearScreen();
    char buffer[STRING_BUFFER_SIZE];

    memset(buffer, 0, sizeof(buffer));
    snprintf(buffer, sizeof(buffer), "Hello.");
    EPD_ShowString(60, 85, buffer, FONT_SIZE_92, BLACK, true);

    memset(buffer, 0, sizeof(buffer));
    snprintf(buffer, sizeof(buffer), "Included font file converter tool.");
    EPD_ShowString(80, 160, buffer, FONT_SIZE_36, BLACK);

    memset(buffer, 0, sizeof(buffer));
    snprintf(buffer, sizeof(buffer),
             "You don't need to build the font from scratch. Just convert it.");
    EPD_ShowString(80, 200, buffer, FONT_SIZE_16, BLACK);

    // Update display
    EPD_Display(imageBW);
    EPD_PartUpdate();
    EPD_DeepSleep();
  }

  void displayMonsterIconsTest() {
    clearScreen();

    // Use safe icon retrieval for all icons
    EPD_drawImage(0, 0, getIcon("emma_cupcake_lg"));
    EPD_drawImage(170, 0, getIcon("emma_mon1_lg"));
    EPD_drawImage(350, 0, getIcon("leo_face_lg"));
    EPD_drawImage(550, 0, getIcon("leo_gator_lg"));

    char buffer[STRING_BUFFER_SIZE];
    memset(buffer, 0, sizeof(buffer));
    snprintf(buffer, sizeof(buffer), "Included monster icons too!");
    EPD_ShowStringRightAligned(790, 250, buffer, FONT_SIZE_36, BLACK);

    // Update display
    EPD_Display(imageBW);
    EPD_PartUpdate();
    EPD_DeepSleep();
  }

  void displayIconsTest() {
    clearScreen();

    int ypos = 0;

    // Update all icon accesses to use getSafeIcon
    EPD_drawImage(0, ypos, getIcon("icon_01d_sm"));
    EPD_drawImage(60, ypos, getIcon("icon_01n_sm"));
    EPD_drawImage(120, ypos, getIcon("icon_02d_sm"));
    EPD_drawImage(180, ypos, getIcon("icon_02n_sm"));
    EPD_drawImage(240, ypos, getIcon("icon_03d_sm"));
    EPD_drawImage(300, ypos, getIcon("icon_03n_sm"));
    EPD_drawImage(360, ypos, getIcon("icon_04d_sm"));
    EPD_drawImage(420, ypos, getIcon("icon_04n_sm"));
    EPD_drawImage(480, ypos, getIcon("icon_09d_sm"));
    EPD_drawImage(540, ypos, getIcon("icon_09n_sm"));
    EPD_drawImage(600, ypos, getIcon("icon_10d_sm"));
    EPD_drawImage(660, ypos, getIcon("icon_10n_sm"));
    EPD_drawImage(720, ypos, getIcon("icon_11d_sm"));

    ypos = 60;
    EPD_drawImage(0, ypos, getIcon("icon_11n_sm"));
    EPD_drawImage(60, ypos, getIcon("icon_13d_sm"));
    EPD_drawImage(120, ypos, getIcon("icon_13n_sm"));

    const char *icons[] = {"wind_sm",    "raindrop_sm", "sunrise_sm",
                           "snow_sm",    "dust_sm",     "rain_sm",
                           "sunset_sm",  "humidity_sm", "barometer_sm",
                           "degrees_sm", "na_md"};

    for (int i = 0; i < sizeof(icons) / sizeof(icons[0]); ++i) {
      EPD_drawImage(200 + i * 30, ypos, getIcon(icons[i]));
    }

    EPD_drawImage(550, 15, getIcon("icon_04n_lg"));
    EPD_drawImage(0, 200, getIcon("error_sm"));

    char buffer[STRING_BUFFER_SIZE];
    memset(buffer, 0, sizeof(buffer));
    snprintf(buffer, sizeof(buffer), "Weather Icons.");
    EPD_ShowStringRightAligned(790, 230, buffer, FONT_SIZE_92, BLACK);

    memset(buffer, 0, sizeof(buffer));
    snprintf(buffer, sizeof(buffer), "Build-in small and large");
    EPD_ShowString(100, 178, buffer, FONT_SIZE_38, BLACK, true);

    // Update display
    EPD_Display(imageBW);
    EPD_PartUpdate();
    EPD_DeepSleep();
  }

  void wirelessOff() {
    WiFi.disconnect(true); // disconnect from WiFi
    WiFi.mode(WIFI_OFF);   // Disable WiFi
    btStop();              // Disable Bluetooth(Just in case)
  }

  static const uint8_t MAX_WEATHER_API_RETRIES = 3;

  /**
   * Calculates the sleep duration based on regular refresh time, time until
   * midnight, and alert start/end times
   * @return sleep duration in microseconds
   */
  uint64_t calculateSleepDuration() {
    // Standard refresh time calculation
    uint64_t minSleepDuration = 1000000ULL * 60ULL * 10; // 10 minutes
    uint64_t requestedSleepDuration = 1000000ULL * 60ULL * refreshMinutes;
    uint64_t regularSleepDuration = (requestedSleepDuration < minSleepDuration)
                                        ? minSleepDuration
                                        : requestedSleepDuration;

    // Default to regular sleep duration
    uint64_t sleepDuration = regularSleepDuration;
    String wakeReason = "Regular refresh interval";

    // Get current time from RTC
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      time_t currentTime = mktime(&timeinfo);

      // Calculate time until next date change (midnight)
      int seconds_to_midnight = (23 - timeinfo.tm_hour) * 3600 +
                                (59 - timeinfo.tm_min) * 60 +
                                (60 - timeinfo.tm_sec);

      // Add 30 seconds delay after midnight
      uint64_t timeToNextDateChange = (seconds_to_midnight + 30) * 1000000ULL;

      // Check if date change is sooner than regular refresh
      if (timeToNextDateChange < sleepDuration) {
        sleepDuration = timeToNextDateChange;
        wakeReason = "Date change (midnight + 30s)";
      }

      // Check for weather alerts
      if (!weatherApiResponse["alerts"].isNull() &&
          weatherApiResponse["alerts"].size() > 0) {
        // Current time in unix timestamp
        time_t now = currentTime;

        // Iterate through alerts to find closest upcoming start or end time
        for (size_t i = 0; i < weatherApiResponse["alerts"].size(); i++) {
          if (!weatherApiResponse["alerts"][i]["start"].isNull()) {
            time_t alertStartTime =
                weatherApiResponse["alerts"][i]["start"].as<long>();

            // If alert starts in the future
            if (alertStartTime > now) {
              uint64_t timeToAlertStart = (alertStartTime - now) * 1000000ULL;

              // If this alert starts sooner than our current wake time
              if (timeToAlertStart < sleepDuration) {
                sleepDuration = timeToAlertStart;
                wakeReason = "Upcoming alert start";
              }
            }
          }

          if (!weatherApiResponse["alerts"][i]["end"].isNull()) {
            time_t alertEndTime =
                weatherApiResponse["alerts"][i]["end"].as<long>();

            // If alert ends in the future
            if (alertEndTime > now) {
              uint64_t timeToAlertEnd = (alertEndTime - now) * 1000000ULL;

              // If this alert ends sooner than our current wake time
              if (timeToAlertEnd < sleepDuration) {
                sleepDuration = timeToAlertEnd;
                wakeReason = "Alert end time";
              }
            }
          }
        }
      }

      // Print current date and time in yyyy-mm-dd hh:mm:ss format
      char currentTimeBuffer[30];
      strftime(currentTimeBuffer, sizeof(currentTimeBuffer),
               "%Y-%m-%d %H:%M:%S", &timeinfo);
      Serial.print("Current date and time: ");
      Serial.println(currentTimeBuffer);

      Serial.print("Sleep duration: ");
      Serial.print((uint32_t)(sleepDuration / 1000000ULL));
      Serial.print(" seconds (");
      Serial.print(wakeReason);
      Serial.println(")");

      return sleepDuration;
    } else {
      // If we can't get the time, use the regular sleep duration
      Serial.println("Failed to get RTC time, using regular sleep duration");
      return regularSleepDuration;
    }
  }

  void ledToggle() {
    if (lowPowerMode)
      return;

    if (ledState) {
      digitalWrite(PWR_LED_PIN, LOW);
      ledState = false;
    } else {
      digitalWrite(PWR_LED_PIN, HIGH);
      ledState = true;
    }
  }

  void ledOff() {
    if (lowPowerMode)
      return;

    digitalWrite(PWR_LED_PIN, LOW);
    ledState = false;
  }

  void ledOn() {
    if (lowPowerMode)
      return;

    digitalWrite(PWR_LED_PIN, HIGH);
    ledState = true;
  }

public:
  uint64_t getSleepDuration() { return calculateSleepDuration(); }

  void begin() {
    errorMessageBuffer = "";
    lowPowerMode = configManager.getLowPowerMode();
    screenPowerOn();
    EPD_GPIOInit();
    if (!lowPowerMode) {
      // when not in low power mode, set output to avoid leakage current.
      pinMode(PWR_LED_PIN, OUTPUT);
    }
  }

  bool run() {
    try {

      // displayIconsTest();
      // displayTypographyTest();
      // displayCustomFontTest();
      // displayMonsterIconsTest();
      // delay(500000);

      // Load configuration from ConfigManager
      loadConfiguration();

      // Check if WiFi is connected (ConfigManager handles connection)
      if (WiFi.status() != WL_CONNECTED) {
        errorMessageBuffer = "WiFi not connected. Check configuration.";
        char title[] = "WiFi Connection Failed";
        char msg[] =
            "Please hold the config button during boot to reconfigure.";
        displayErrorMessage(title, msg);
        return false;
      }

      // Resolve location (manual override takes precedence over cached GeoJS)
      if (!resolveLocationWithGeoIp()) {
        char title[] = "Location Lookup Failed";
        char msg[256];
        memset(msg, 0, sizeof(msg));
        snprintf(msg, sizeof(msg), "%s", errorMessageBuffer.c_str());
        displayErrorMessage(title, msg);
        return false;
      }

      if (!getWeatherInfo(MAX_WEATHER_API_RETRIES)) {
        char title[] = "Weather API failed.";
        char msg[256];
        memset(msg, 0, sizeof(msg));
        snprintf(msg, sizeof(msg), "%s\n\n(After %d retry attempts)",
                 errorMessageBuffer.c_str(), MAX_WEATHER_API_RETRIES);
        displayErrorMessage(title, msg);
        return false;
      } else {
        logPrintln("Weather information retrieved successfully.");
        displayWeatherForecast();
        return true;
      }
    } catch (const std::exception &e) {
      errorMessageBuffer = String("Exception: ") + e.what();
      displayError();
    }
    // never reach here
    return false;
  }
};

WeatherCrow weatherCrow;

/**
 * Display configuration portal instructions on e-paper screen
 * Called by ConfigManager before starting the access point
 * Screen resolution: 792w x 272h
 */
void displayConfigPortalScreen(const char *apName, const char *ipAddress,
                               const char *reason) {
  Serial.println("[Display] Showing config portal instructions");
  Serial.print("[Display] Reason: ");
  Serial.println(reason);

  // Initialize display
  const int SCREEN_POWER_PIN = 7;
  pinMode(SCREEN_POWER_PIN, OUTPUT);
  digitalWrite(SCREEN_POWER_PIN, HIGH);
  delay(100);

  EPD_GPIOInit();
  Paint_NewImage(configImageBW, EPD_W, EPD_H, Rotation, WHITE);
  Paint_Clear(WHITE);
  EPD_Init();

  char buffer[128];

  EPD_drawImage(10, 20, icon_11d_lg);

  uint16_t baseXpos = 288;

  // Display reason (e.g., "WiFi Connection Failed" or "Manual Setup")
  EPD_ShowString(baseXpos, 28, reason, FONT_SIZE_16, BLACK);

  EPD_ShowString(baseXpos, 60, "Connect to WiFi", FONT_SIZE_16, BLACK);

  memset(buffer, 0, sizeof(buffer));
  snprintf(buffer, sizeof(buffer), "%s", apName);
  EPD_ShowString(baseXpos, 95, buffer, FONT_SIZE_36, BLACK);

  EPD_ShowString(baseXpos, 135, "Access the URL to configure", FONT_SIZE_16,
                 BLACK);

  memset(buffer, 0, sizeof(buffer));
  snprintf(buffer, sizeof(buffer), "http://%s", ipAddress);
  EPD_ShowString(baseXpos, 165, buffer, FONT_SIZE_36, BLACK);

  EPD_ShowString(baseXpos, 225,
                 "Timeout: 5 min | Hold config button on boot to re-enter",
                 FONT_SIZE_16, BLACK);
  EPD_ShowString(baseXpos, 255, "Device restarts after save.", FONT_SIZE_16,
                 BLACK);

  // Update display
  EPD_Display(configImageBW);
  EPD_Update();
  EPD_DeepSleep();

  // Blink LED to indicate AP mode is active
  pinMode(PWR_LED_PIN, OUTPUT);
  for (int i = 0; i < 5; i++) {
    digitalWrite(PWR_LED_PIN, HIGH);
    delay(200);
    digitalWrite(PWR_LED_PIN, LOW);
    delay(200);
  }
  // Leave LED on to indicate AP mode
  digitalWrite(PWR_LED_PIN, HIGH);

  Serial.println("[Display] Config portal screen displayed");
}

void setup() {
  // Initialize Serial first for debug output
  Serial.begin(BAUD_RATE);
  delay(1000);
  Serial.println("\n\n=== Weather Crow Starting ===");

  // Set callback to display config portal instructions on e-paper
  configManager.setPortalStartCallback(displayConfigPortalScreen);

  // Initialize ConfigManager - this handles WiFi connection
  // If config button is held, it will show the config portal
  bool wifiConnected = configManager.begin(CONFIG_BUTTON_PIN);

  // Get low power mode from config
  bool lowPowerMode = configManager.getLowPowerMode();

  if (lowPowerMode) {
    setCpuFrequencyMhz(80);
    btStop(); // Disable Bluetooth, as it is not used in this project
    Serial.println("[Setup] Low power mode enabled");
  }

  weatherCrow.begin();

  if (!wifiConnected) {
    Serial.println("[Setup] WiFi not connected - will retry in loop");
  }
}

void loop() {

  Serial.println("Starting weatherCrow.run()");
  if (weatherCrow.run() == true) {
    Serial.println("weatherCrow.run() completed successfully");
    uint64_t sleepDuration = weatherCrow.getSleepDuration();
    esp_sleep_enable_timer_wakeup(sleepDuration);
    esp_deep_sleep_start();
  } else {
    // refresh failed, wait for 5 minutes and try again.
    esp_sleep_enable_timer_wakeup(1000000ULL * 60ULL * 5);
    esp_deep_sleep_start();
  }
}
