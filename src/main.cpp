// Ham shack clock for an ESP32 "Cheap Yellow Display": local/UTC time,
// N0NBH solar and band conditions, and a live gray-line map. Tap the
// screen to change pages. Callsign, location and time zone are set on a
// web page served by the clock itself.

#include <Arduino.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <XPT2046_Touchscreen.h>
#include <math.h>
#include <time.h>

#include "config.h"
#include "landmask.h"

// CYD wiring: touch controller on its own SPI bus, RGB LED (active low).
#define TOUCH_CS   33
#define TOUCH_IRQ  36
#define TOUCH_SCK  25
#define TOUCH_MISO 39
#define TOUCH_MOSI 32
#define LED_R      4
#define LED_G      16
#define LED_B      17

#define W 320
#define H 240

// Colours (RGB565).
#define C_BG       0x0000
#define C_PANEL    0x10A2
#define C_HEADER   0x0190
#define C_TEXT     0xFFFF
#define C_DIM      0x8C71
#define C_GREEN    0x05E0
#define C_RED      0xC000
#define C_ORANGE   0xFC00
#define C_YELLOW   0xFFE0
#define C_CYAN     0x07FF

// Map colours: day and night versions of ocean and land.
#define M_OCEAN_DAY   0x2B5F
#define M_LAND_DAY    0x5D2A
#define M_OCEAN_NIGHT 0x0006
#define M_LAND_NIGHT  0x10E2

TFT_eSPI tft;
SPIClass touchSpi(VSPI);
XPT2046_Touchscreen touch(TOUCH_CS, TOUCH_IRQ);

enum Page { CLOCK, BANDS, MAP, PAGES };
static const char* PAGE_NAMES[] = {"", "Band conditions", "Gray line"};

// N0NBH solar data (written by the fetch task, read by the UI).
struct Solar {
  bool ok = false;
  String updated, sfi, ssn, a, k, xray, wind, aurora, geomag, noise, muf, esNA;
  String day[4], night[4];  // 80-40, 30-20, 17-15, 12-10
};
static const char* BANDS_XML[] = {"80m-40m", "30m-20m", "17m-15m", "12m-10m"};
static const char* BANDS_SHORT[] = {"80-40m", "30-20m", "17-15m", "12-10m"};

Solar solar;
SemaphoreHandle_t lock;
volatile bool solarChanged = true;

// ---------- settings ----------

struct Settings {
  String call = DEFAULT_CALLSIGN;
  String location = DEFAULT_LOCATION;  // as typed: grid square or "lat,lon"
  String tz = DEFAULT_TIMEZONE;        // POSIX TZ string
  bool h24 = false;
  int cycle = 0;  // seconds per page; 0 = only on tap
  // Derived from `location`.
  bool hasLoc = false;
  double lat = 0, lon = 0;
};
Settings cfg;
volatile bool settingsChanged = false;

struct Zone { const char* name; const char* tz; };
static const Zone ZONES[] = {
    {"US Eastern", "EST5EDT,M3.2.0,M11.1.0"},
    {"US Central", "CST6CDT,M3.2.0,M11.1.0"},
    {"US Mountain", "MST7MDT,M3.2.0,M11.1.0"},
    {"US Arizona", "MST7"},
    {"US Pacific", "PST8PDT,M3.2.0,M11.1.0"},
    {"US Alaska", "AKST9AKDT,M3.2.0,M11.1.0"},
    {"US Hawaii", "HST10"},
    {"Canada Atlantic", "AST4ADT,M3.2.0,M11.1.0"},
    {"Canada Newfoundland", "NST3:30NDT,M3.2.0,M11.1.0"},
    {"UK / Ireland", "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Central Europe", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Eastern Europe", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"India", "IST-5:30"},
    {"Japan", "JST-9"},
    {"Australia Eastern", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"New Zealand", "NZST-12NZDT,M9.5.0,M4.1.0/3"},
    {"UTC", "UTC0"},
};

// Grid square (4 or 6 characters, centre of the square) or "lat,lon".
static bool parseLocation(String text, double& lat, double& lon) {
  text.trim();
  int comma = text.indexOf(',');
  if (comma > 0) {
    lat = text.substring(0, comma).toDouble();
    lon = text.substring(comma + 1).toDouble();
    return fabs(lat) <= 90 && fabs(lon) <= 180 && (lat != 0 || lon != 0);
  }
  text.toUpperCase();
  int n = text.length();
  if (n != 4 && n != 6) return false;
  if (text[0] < 'A' || text[0] > 'R' || text[1] < 'A' || text[1] > 'R') return false;
  if (!isdigit(text[2]) || !isdigit(text[3])) return false;
  lon = (text[0] - 'A') * 20 + (text[2] - '0') * 2 - 180;
  lat = (text[1] - 'A') * 10 + (text[3] - '0') - 90;
  if (n == 6) {
    if (text[4] < 'A' || text[4] > 'X' || text[5] < 'A' || text[5] > 'X') return false;
    lon += (text[4] - 'A') / 12.0 + 1 / 24.0;
    lat += (text[5] - 'A') / 24.0 + 1 / 48.0;
  } else {
    lon += 1;
    lat += 0.5;
  }
  return true;
}

static void applyLocation() {
  cfg.hasLoc = parseLocation(cfg.location, cfg.lat, cfg.lon);
}

static void loadSettings() {
  Preferences p;
  p.begin("hamclock", false);  // read-write creates the namespace on first boot
  // isKey first: getString on a missing key logs an error.
  if (p.isKey("call")) cfg.call = p.getString("call");
  if (p.isKey("loc")) cfg.location = p.getString("loc");
  if (p.isKey("tz")) cfg.tz = p.getString("tz");
  cfg.h24 = p.getBool("h24", false);
  cfg.cycle = p.getInt("cycle", 0);
  p.end();
  applyLocation();
}

static bool configured() {
  Preferences p;
  p.begin("hamclock", false);
  bool saved = p.isKey("call");
  p.end();
  return saved;
}

// Setup-page fields. The two <select>s are custom HTML rebuilt into fixed
// buffers, so WiFiManager's pointers to them stay valid.
WiFiManager wm;
char callBuf[16], locBuf[32], cycleBuf[8];
WiFiManagerParameter pCall("call", "Callsign", "", 12);
WiFiManagerParameter pLoc("loc", "Location: grid square (FN31pr) or lat,lon (41.71,-72.73)", "", 30);
static char tzHtml[2200], fmtHtml[300];
WiFiManagerParameter pTz(tzHtml);
WiFiManagerParameter pFmt(fmtHtml);
WiFiManagerParameter pCycle("cycle", "Change page every N seconds (0 = only when tapped)", "", 5);

static void buildParams() {
  String h = "<br><label for='tz'>Time zone</label><select name='tz' id='tz'>";
  bool found = false;
  for (const Zone& z : ZONES) {
    bool sel = cfg.tz == z.tz;
    found |= sel;
    h += String("<option value='") + z.tz + "'" + (sel ? " selected" : "") + ">" + z.name + "</option>";
  }
  if (!found) h += "<option value='" + cfg.tz + "' selected>Custom: " + cfg.tz + "</option>";
  h += "</select>";
  strlcpy(tzHtml, h.c_str(), sizeof tzHtml);
  snprintf(fmtHtml, sizeof fmtHtml,
           "<br><label for='h24'>Clock</label><select name='h24' id='h24'>"
           "<option value='0'%s>12-hour (3:45 PM)</option><option value='1'%s>24-hour (15:45)</option></select>",
           cfg.h24 ? "" : " selected", cfg.h24 ? " selected" : "");
  pCall.setValue(cfg.call.c_str(), 12);
  pLoc.setValue(cfg.location.c_str(), 30);
  pCycle.setValue(String(cfg.cycle).c_str(), 5);
}

static void saveParams() {
  String call = pCall.getValue();
  call.trim();
  call.toUpperCase();
  if (call.length()) cfg.call = call;
  cfg.location = pLoc.getValue();
  cfg.location.trim();
  if (wm.server->hasArg("tz")) cfg.tz = wm.server->arg("tz");
  if (wm.server->hasArg("h24")) cfg.h24 = wm.server->arg("h24") == "1";
  cfg.cycle = constrain(String(pCycle.getValue()).toInt(), 0, 3600);
  applyLocation();

  Preferences p;
  p.begin("hamclock", false);
  p.putString("call", cfg.call);
  p.putString("loc", cfg.location);
  p.putString("tz", cfg.tz);
  p.putBool("h24", cfg.h24);
  p.putInt("cycle", cfg.cycle);
  p.end();
  Serial.printf("settings saved: %s %s (%s) tz=%s\n", cfg.call.c_str(), cfg.location.c_str(),
                cfg.hasLoc ? "ok" : "not set", cfg.tz.c_str());
  setenv("TZ", cfg.tz.c_str(), 1);
  tzset();
  buildParams();
  settingsChanged = true;
}

// ---------- solar data ----------

// Text inside the first <tag>...</tag>, trimmed.
static String tagText(const String& xml, const char* tag) {
  String open = String("<") + tag + ">";
  int a = xml.indexOf(open);
  if (a < 0) return "";
  a += open.length();
  int b = xml.indexOf("</", a);
  String s = xml.substring(a, b);
  s.trim();
  return s;
}

// Text of the element whose opening tag contains `attrs`, e.g.
// band name="80m-40m" time="day".
static String elementText(const String& xml, const String& attrs) {
  int a = xml.indexOf(attrs);
  if (a < 0) return "";
  a = xml.indexOf('>', a) + 1;
  int b = xml.indexOf("</", a);
  String s = xml.substring(a, b);
  s.trim();
  return s;
}

static bool fetchSolar() {
  WiFiClientSecure client;
  client.setInsecure();  // public, read-only data
  HTTPClient http;
  http.setTimeout(15000);
  if (!http.begin(client, "https://www.hamqsl.com/solarxml.php")) return false;
  http.setUserAgent(USER_AGENT);
  int code = http.GET();
  String xml = code == 200 ? http.getString() : "";
  http.end();
  if (code != 200 || xml.indexOf("<solarflux>") < 0) {
    Serial.printf("solar data: HTTP %d\n", code);
    return false;
  }
  Solar s;
  s.ok = true;
  s.updated = tagText(xml, "updated");
  s.sfi = tagText(xml, "solarflux");
  s.ssn = tagText(xml, "sunspots");
  s.a = tagText(xml, "aindex");
  s.k = tagText(xml, "kindex");
  s.xray = tagText(xml, "xray");
  s.wind = tagText(xml, "solarwind");
  s.aurora = tagText(xml, "aurora");
  s.geomag = tagText(xml, "geomagfield");
  s.noise = tagText(xml, "signalnoise");
  s.muf = tagText(xml, "muf");
  s.esNA = elementText(xml, "location=\"north_america\"");
  for (int i = 0; i < 4; i++) {
    s.day[i] = elementText(xml, String("name=\"") + BANDS_XML[i] + "\" time=\"day\"");
    s.night[i] = elementText(xml, String("name=\"") + BANDS_XML[i] + "\" time=\"night\"");
  }
  xSemaphoreTake(lock, portMAX_DELAY);
  solar = s;
  xSemaphoreGive(lock);
  solarChanged = true;
  return true;
}

static void fetchTask(void*) {
  for (;;) {
    bool ok = WiFi.status() == WL_CONNECTED && fetchSolar();
    vTaskDelay(pdMS_TO_TICKS(ok ? SOLAR_EVERY_MS : SOLAR_RETRY_MS));
  }
}

// ---------- astronomy ----------

static double rad(double d) { return d * M_PI / 180; }
static double deg(double r) { return r * 180 / M_PI; }

// Sun's declination and the longitude where it's overhead, in degrees.
static void sunPosition(time_t t, double& dec, double& subLon) {
  double d = t / 86400.0 - 10957.5;  // days since J2000.0
  double g = rad(fmod(357.529 + 0.98560028 * d, 360));
  double q = fmod(280.459 + 0.98564736 * d, 360);
  double L = rad(q + 1.915 * sin(g) + 0.020 * sin(2 * g));
  double e = rad(23.439 - 0.00000036 * d);
  double ra = atan2(cos(e) * sin(L), cos(L));
  dec = deg(asin(sin(e) * sin(L)));
  double gmst = fmod(18.697374558 + 24.06570982441908 * d, 24);
  subLon = fmod(deg(ra) - gmst * 15 + 540, 360) - 180;
}

// Sun elevation in degrees at a place and time.
static double sunElevation(time_t t, double lat, double lon) {
  double dec, subLon;
  sunPosition(t, dec, subLon);
  double c = sin(rad(lat)) * sin(rad(dec)) + cos(rad(lat)) * cos(rad(dec)) * cos(rad(lon - subLon));
  return deg(asin(c));
}

// Today's local sunrise and sunset at the QTH (0 if none), found by
// scanning minute by minute for the -0.833 degree crossing.
static void sunTimes(time_t now, time_t& rise, time_t& set) {
  struct tm lt;
  localtime_r(&now, &lt);
  lt.tm_hour = 0;
  lt.tm_min = 0;
  lt.tm_sec = 0;
  time_t start = mktime(&lt);
  rise = set = 0;
  double prev = sunElevation(start, cfg.lat, cfg.lon) + 0.833;
  for (int m = 1; m <= 1440; m++) {
    time_t t = start + m * 60;
    double el = sunElevation(t, cfg.lat, cfg.lon) + 0.833;
    if (prev < 0 && el >= 0 && !rise) rise = t;
    if (prev >= 0 && el < 0 && !set) set = t;
    prev = el;
  }
}

static String gridSquare(double lat, double lon) {
  lon += 180;
  lat += 90;
  char g[7];
  g[0] = 'A' + int(lon / 20);
  g[1] = 'A' + int(lat / 10);
  g[2] = '0' + int(fmod(lon, 20) / 2);
  g[3] = '0' + int(fmod(lat, 10));
  g[4] = 'a' + int(fmod(lon, 2) * 12);
  g[5] = 'a' + int(fmod(lat, 1) * 24);
  g[6] = 0;
  return g;
}

// ---------- drawing helpers ----------

static bool timeValid() { return time(nullptr) > 1700000000; }

static String clockText(time_t t) {
  struct tm lt;
  localtime_r(&t, &lt);
  char buf[16];
  strftime(buf, sizeof buf, cfg.h24 ? "%H:%M" : "%l:%M %p", &lt);
  String s = buf;
  s.trim();
  return s;
}

static uint16_t conditionColor(const String& c) {
  if (c == "Good") return C_GREEN;
  if (c == "Fair") return C_YELLOW;
  if (c == "Poor") return C_RED;
  return C_DIM;
}

static uint16_t kColor(const String& k) {
  int v = k.toInt();
  if (!k.length()) return C_DIM;
  return v >= 5 ? C_RED : v >= 3 ? C_YELLOW : C_GREEN;
}

static void header(Page p) {
  tft.fillRect(0, 0, W, 26, C_HEADER);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(C_TEXT, C_HEADER);
  tft.drawString(cfg.call, 6, 13, 4);
  tft.setTextColor(C_DIM, C_HEADER);
  if (cfg.hasLoc) tft.drawString(gridSquare(cfg.lat, cfg.lon), tft.textWidth(cfg.call, 4) + 14, 14, 2);
  tft.setTextDatum(MR_DATUM);
  tft.setTextColor(C_TEXT, C_HEADER);
  tft.drawString(PAGE_NAMES[p], W - 40, 14, 2);
  for (int i = 0; i < PAGES; i++) tft.fillCircle(W - 28 + i * 9, 13, 3, i == p ? C_TEXT : C_DIM);
}

static void tile(int x, int y, int w, int h, const char* label, const String& value, uint16_t color) {
  tft.fillRoundRect(x, y, w, h, 6, C_PANEL);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(C_DIM, C_PANEL);
  tft.drawString(label, x + w / 2, y + 3, 2);
  tft.setTextColor(color, C_PANEL);
  tft.drawString(value.length() ? value : "-", x + w / 2, y + 19, 4);
}

// ---------- pages ----------

// The parts of the clock page that change every second.
static void drawClockTime() {
  time_t now = time(nullptr);
  tft.setTextColor(C_TEXT, C_BG);
  if (!timeValid()) {
    tft.setTextDatum(TL_DATUM);
    tft.drawString("Getting time...", 10, 40, 4);
    return;
  }
  struct tm lt, ut;
  localtime_r(&now, &lt);
  gmtime_r(&now, &ut);
  char hm[8], ss[4], date[32], utc[16];
  strftime(hm, sizeof hm, cfg.h24 ? "%H:%M" : "%l:%M", &lt);
  strftime(ss, sizeof ss, "%S", &lt);
  strftime(date, sizeof date, "%a %b %e, %Y", &lt);
  strftime(utc, sizeof utc, "%H:%M", &ut);

  // Local time in 7-segment digits, seconds and AM/PM beside it. These end
  // by x=224, clear of the UTC column (x >= 244).
  tft.setTextDatum(TR_DATUM);
  tft.setTextPadding(180);
  tft.drawString(hm, 182, 34, 7);
  tft.setTextPadding(0);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_DIM, C_BG);
  tft.drawString(ss, 188, 36, 4);
  tft.setTextColor(C_TEXT, C_BG);
  tft.drawString(cfg.h24 ? "  " : lt.tm_hour >= 12 ? "PM" : "AM", 188, 62, 4);

  tft.setTextPadding(170);
  tft.setTextColor(C_DIM, C_BG);
  tft.drawString(date, 10, 90, 2);
  tft.setTextPadding(0);

  // UTC.
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(C_CYAN, C_BG);
  tft.setTextPadding(70);
  tft.drawString(utc, W - 6, 40, 4);
  tft.setTextPadding(0);
  tft.setTextColor(C_DIM, C_BG);
  tft.drawString("UTC", W - 6, 66, 2);
  struct tm u2 = ut;
  char ud[8];
  strftime(ud, sizeof ud, "%d %b", &u2);
  tft.setTextPadding(60);
  tft.drawString(ud, W - 6, 82, 2);
  tft.setTextPadding(0);
}

static void drawClockPage(const Solar& s) {
  tft.fillScreen(C_BG);
  header(CLOCK);
  drawClockTime();

  tile(4, 110, 75, 46, "SFI", s.sfi, C_TEXT);
  tile(83, 110, 75, 46, "Sunspots", s.ssn, C_TEXT);
  tile(162, 110, 75, 46, "A index", s.a, C_TEXT);
  tile(241, 110, 75, 46, "K index", s.k, kColor(s.k));

  // Band conditions for right now (day or night at the QTH).
  // Without a location, assume daytime 7 AM - 7 PM local.
  bool day;
  if (cfg.hasLoc) {
    day = timeValid() && sunElevation(time(nullptr), cfg.lat, cfg.lon) > 0;
  } else {
    time_t now = time(nullptr);
    struct tm lt;
    localtime_r(&now, &lt);
    day = lt.tm_hour >= 7 && lt.tm_hour < 19;
  }
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_DIM, C_BG);
  tft.drawString(day ? "HF now (day)" : "HF now (night)", 6, 162, 2);
  if (s.ok) {
    tft.setTextDatum(TR_DATUM);
    tft.drawString("noise " + s.noise, W - 6, 162, 2);
  }
  for (int i = 0; i < 4; i++) {
    const String& c = day ? s.day[i] : s.night[i];
    int x = 4 + i * 79;
    uint16_t col = conditionColor(c);
    tft.fillRoundRect(x, 182, 75, 54, 6, col);
    uint16_t fg = col == C_YELLOW ? C_BG : C_TEXT;
    tft.setTextColor(fg, col);
    tft.setTextDatum(TC_DATUM);
    tft.drawString(BANDS_SHORT[i], x + 37, 187, 2);
    tft.drawString(c.length() ? c : "-", x + 37, 206, 4);
  }
}

static void drawBandsPage(const Solar& s) {
  tft.fillScreen(C_BG);
  header(BANDS);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(C_DIM, C_BG);
  tft.drawString("Day", 160, 38, 2);
  tft.drawString("Night", 260, 38, 2);
  for (int i = 0; i < 4; i++) {
    int y = 48 + i * 30;
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(C_TEXT, C_BG);
    tft.drawString(BANDS_SHORT[i], 10, y + 13, 4);
    for (int j = 0; j < 2; j++) {
      const String& c = j ? s.night[i] : s.day[i];
      uint16_t col = conditionColor(c);
      int x = 115 + j * 100;
      tft.fillRoundRect(x, y, 92, 26, 5, col);
      tft.setTextDatum(MC_DATUM);
      tft.setTextColor(col == C_YELLOW ? C_BG : C_TEXT, col);
      tft.drawString(c.length() ? c : "-", x + 46, y + 13, 2);
    }
  }
  // Space weather details in two columns.
  struct Row { const char* label; String value; };
  Row rows[] = {
      {"Geomag", s.geomag}, {"Noise", s.noise}, {"X-ray", s.xray},
      {"Wind", s.wind.length() ? s.wind + " km/s" : ""}, {"Aurora", s.aurora}, {"E-skip NA", s.esNA},
  };
  tft.setTextDatum(TL_DATUM);
  for (int i = 0; i < 6; i++) {
    int x = i % 2 ? 165 : 8, y = 172 + (i / 2) * 18;
    tft.setTextColor(C_DIM, C_BG);
    tft.drawString(rows[i].label, x, y, 2);
    tft.setTextColor(C_TEXT, C_BG);
    tft.drawString(rows[i].value.length() ? rows[i].value : "-", x + 68, y, 2);
  }
  tft.setTextColor(C_DIM, C_BG);
  tft.setTextDatum(BR_DATUM);
  tft.drawString(s.ok ? "N0NBH " + s.updated : "Waiting for hamqsl.com", W - 4, H - 1, 1);
  tft.setTextDatum(BL_DATUM);
  tft.drawString("Settings: http://" + WiFi.localIP().toString(), 4, H - 1, 1);
}

static void drawMapPage() {
  tft.fillScreen(C_BG);
  header(MAP);
  const int top = 30;
  time_t now = time(nullptr);
  double dec, subLon;
  sunPosition(now, dec, subLon);
  double sd = sin(rad(dec)), cd = cos(rad(dec));
  static double cosH[MAP_W];
  for (int x = 0; x < MAP_W; x++) cosH[x] = cos(rad(-180 + (x + 0.5) * 360.0 / MAP_W - subLon));

  uint16_t line[MAP_W];
  for (int y = 0; y < MAP_H; y++) {
    double lat = rad(90 - (y + 0.5) * 180.0 / MAP_H);
    double sl = sin(lat) * sd, cl = cos(lat) * cd;
    for (int x = 0; x < MAP_W; x++) {
      int i = y * MAP_W + x;
      bool land = pgm_read_byte(&LAND_MASK[i >> 3]) & (0x80 >> (i & 7));
      double el = deg(asin(sl + cl * cosH[x]));
      // Full day above 0 degrees, full night below -12 (nautical twilight).
      double t = el >= 0 ? 1 : el <= -12 ? 0 : (el + 12) / 12;
      uint8_t alpha = uint8_t(t * 255);
      line[x] = land ? tft.alphaBlend(alpha, M_LAND_DAY, M_LAND_NIGHT) : tft.alphaBlend(alpha, M_OCEAN_DAY, M_OCEAN_NIGHT);
    }
    tft.pushImage(0, top + y, MAP_W, 1, line);
  }

  // Sun overhead point and the station.
  int sx = int((subLon + 180) / 360 * MAP_W), sy = top + int((90 - dec) / 180 * MAP_H);
  tft.fillCircle(sx, sy, 5, C_YELLOW);
  tft.drawCircle(sx, sy, 6, C_ORANGE);
  int y = top + MAP_H + 8;
  if (!cfg.hasLoc) {
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(C_DIM, C_BG);
    tft.drawString("Set your location at http://" + WiFi.localIP().toString(), 8, y, 2);
    return;
  }
  int qx = int((cfg.lon + 180) / 360 * MAP_W), qy = top + int((90 - cfg.lat) / 180 * MAP_H);
  tft.drawLine(qx - 5, qy, qx + 5, qy, C_RED);
  tft.drawLine(qx, qy - 5, qx, qy + 5, C_RED);
  tft.fillCircle(qx, qy, 2, C_TEXT);

  // Sunrise/sunset at the station.
  time_t rise, set;
  sunTimes(now, rise, set);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_DIM, C_BG);
  tft.drawString("Sunrise", 8, y, 2);
  tft.drawString("Sunset", 168, y, 2);
  tft.setTextColor(C_YELLOW, C_BG);
  tft.drawString(rise ? clockText(rise) : "-", 64, y - 4, 4);
  tft.setTextColor(C_ORANGE, C_BG);
  tft.drawString(set ? clockText(set) : "-", 220, y - 4, 4);
  tft.setTextColor(C_DIM, C_BG);
  tft.setTextDatum(BL_DATUM);
  double el = sunElevation(now, cfg.lat, cfg.lon);
  tft.drawString("Sun at QTH " + String(el, 0) + " deg", 8, H - 1, 1);
}

static void message(const String& title, const String& body) {
  tft.fillScreen(C_BG);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_YELLOW, C_BG);
  tft.drawString(title, 10, 20, 4);
  tft.setTextColor(C_TEXT, C_BG);
  // Simple word wrap in font 2.
  String line, word;
  int y = 60;
  for (size_t i = 0; i <= body.length(); i++) {
    char c = i < body.length() ? body[i] : ' ';
    if (c != ' ') { word += c; continue; }
    String trial = line.length() ? line + " " + word : word;
    if (tft.textWidth(trial, 2) > W - 20 && line.length()) {
      tft.drawString(line, 10, y, 2);
      y += 18;
      line = word;
    } else {
      line = trial;
    }
    word = "";
  }
  if (line.length()) tft.drawString(line, 10, y, 2);
}

// ---------- setup / loop ----------

void setup() {
  Serial.begin(115200);
  for (int p : {LED_R, LED_G, LED_B}) {
    pinMode(p, OUTPUT);
    digitalWrite(p, HIGH);  // LED off
  }

  tft.init();
  tft.setRotation(1);
  tft.setSwapBytes(true);
  touchSpi.begin(TOUCH_SCK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  touch.begin(touchSpi);
  touch.setRotation(1);

  loadSettings();
  message("Ham Clock", "Starting... Touch and hold the screen now to open the setup page.");
  uint32_t t0 = millis();
  bool openPortal = !configured();  // first start: always ask for callsign and location
  while (millis() - t0 < 2500 && !openPortal) {
    if (touch.touched()) openPortal = true;
    delay(20);
  }

  buildParams();
  wm.addParameter(&pCall);
  wm.addParameter(&pLoc);
  wm.addParameter(&pTz);
  wm.addParameter(&pFmt);
  wm.addParameter(&pCycle);
  wm.setSaveParamsCallback(saveParams);
  wm.setTitle("Ham Clock");
  // No separate "param" page during first setup: WiFiManager then puts the
  // callsign/location fields on the Wi-Fi page, so they're saved together.
  std::vector<const char*> setupMenu = {"wifi", "info", "restart"};
  wm.setMenu(setupMenu);
  wm.setAPCallback([](WiFiManager*) {
    message("Setup",
            "On your phone, join the Wi-Fi network \"" SETUP_AP_NAME "\". A setup page opens "
            "(or browse to 192.168.4.1). Tap Configure WiFi, pick your network, and fill in "
            "your callsign, location and time zone.");
  });
  wm.setConnectTimeout(20);
  if (openPortal) {
    wm.startConfigPortal(SETUP_AP_NAME);
  } else {
    message("Connecting to Wi-Fi...", "");
    wm.autoConnect(SETUP_AP_NAME);
  }
  message("Connected", WiFi.SSID() + "  " + WiFi.localIP().toString() +
                           "   Change settings any time at http://" + WiFi.localIP().toString());
  delay(1500);

  // Keep the settings page available on the home network, Setup first.
  std::vector<const char*> homeMenu = {"param", "wifi", "info", "restart"};
  wm.setMenu(homeMenu);
  wm.setConfigPortalBlocking(false);
  wm.startWebPortal();

  configTzTime(cfg.tz.c_str(), "pool.ntp.org", "time.nist.gov");
  lock = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(fetchTask, "fetch", 12288, nullptr, 1, nullptr, 0);
}

void loop() {
  static Page page = CLOCK;
  static bool redraw = true;
  static uint32_t lastTouch = 0, pageSince = 0, mapDrawn = 0;
  static time_t lastSecond = 0;
  static bool hadTime = false;

  wm.process();
  if (settingsChanged) {
    settingsChanged = false;
    pageSince = millis();
    redraw = true;
  }

  if (touch.touched() && millis() - lastTouch > 400) {
    lastTouch = millis();
    page = Page((page + 1) % PAGES);
    pageSince = millis();
    redraw = true;
  }
  if (cfg.cycle && millis() - pageSince > cfg.cycle * 1000UL) {
    page = Page((page + 1) % PAGES);
    pageSince = millis();
    redraw = true;
  }
  if (solarChanged && page != MAP) redraw = true;
  solarChanged = false;
  // Redraw once the clock first sets (sun position and day/night depend on it).
  if (timeValid() && !hadTime) {
    hadTime = true;
    redraw = true;
  }
  if (page == MAP && millis() - mapDrawn > 120000) redraw = true;

  if (redraw) {
    redraw = false;
    xSemaphoreTake(lock, portMAX_DELAY);
    Solar s = solar;
    xSemaphoreGive(lock);
    if (page == CLOCK) drawClockPage(s);
    else if (page == BANDS) drawBandsPage(s);
    else { drawMapPage(); mapDrawn = millis(); }
  }

  time_t now = time(nullptr);
  if (page == CLOCK && now != lastSecond) {
    lastSecond = now;
    drawClockTime();
    // The day/night band summary flips at sunrise and sunset.
    struct tm lt;
    localtime_r(&now, &lt);
    if (lt.tm_sec == 0 && lt.tm_min % 10 == 0) redraw = true;
  }
  delay(20);
}
