/**
 * Google Apps Script for Weather Crow 5.7 Calendar Display
 *
 * Setup Instructions:
 * 1. Open https://script.google.com and click "New project".
 * 2. Replace the contents of Code.gs with this script and click Save.
 * 3. (Optional) Set CALENDAR_ID or CALENDAR_NAME below to use a shared calendar.
 * 4. Click "Deploy" -> "New deployment" (or "Manage deployments" -> Edit -> "New version").
 * 5. Select type: "Web app", Execute as: "Me", Who has access: "Anyone".
 * 6. Copy the Web App URL into DEFAULT_CALENDAR_URL in src/ConfigManager.h
 *    (or into the device's configuration portal at http://192.168.4.1).
 */

// Option A: Specify a shared Calendar ID (from Calendar Settings -> Integrate calendar)
// Example: "abcdef123456@group.calendar.google.com"
var CALENDAR_ID = "";

// Option B: Or specify the exact display name of the shared calendar
// Example: "Family Calendar"
var CALENDAR_NAME = "";

function getTargetCalendar() {
  if (CALENDAR_ID && CALENDAR_ID.length > 0) {
    var byId = CalendarApp.getCalendarById(CALENDAR_ID);
    if (byId) return byId;
  }
  if (CALENDAR_NAME && CALENDAR_NAME.length > 0) {
    var matches = CalendarApp.getCalendarsByName(CALENDAR_NAME);
    if (matches && matches.length > 0) return matches[0];
  }
  return CalendarApp.getDefaultCalendar();
}

function doGet(e) {
  var daysAhead = 7;
  var maxEvents = 8;
  var now = new Date();
  var end = new Date(now.getTime() + daysAhead * 24 * 60 * 60 * 1000);
  var tz = Session.getScriptTimeZone();

  var cal = getTargetCalendar();
  if (!cal) {
    return ContentService
      .createTextOutput(JSON.stringify({ events: [], error: "Calendar not found" }))
      .setMimeType(ContentService.MimeType.JSON);
  }

  var rawEvents = cal.getEvents(now, end);

  var todayStr = Utilities.formatDate(now, tz, "yyyy-MM-dd");
  var tmrw = new Date(now.getTime() + 24 * 60 * 60 * 1000);
  var tmrwStr = Utilities.formatDate(tmrw, tz, "yyyy-MM-dd");

  var events = [];
  for (var i = 0; i < rawEvents.length && events.length < maxEvents; i++) {
    var ev = rawEvents[i];
    var start = ev.getStartTime();
    var evDayStr = Utilities.formatDate(start, tz, "yyyy-MM-dd");

    var dateLabel = Utilities.formatDate(start, tz, "EEE MMM d");
    if (evDayStr === todayStr) {
      dateLabel = "Today";
    } else if (evDayStr === tmrwStr) {
      dateLabel = "Tmrw";
    }

    var timeLabel = ev.isAllDayEvent()
      ? "All Day"
      : Utilities.formatDate(start, tz, "h:mm a");

    events.push({
      date: dateLabel,
      time: timeLabel,
      title: ev.getTitle() || "(No title)"
    });
  }

  return ContentService
    .createTextOutput(JSON.stringify({ events: events }))
    .setMimeType(ContentService.MimeType.JSON);
}
