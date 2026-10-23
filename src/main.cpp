#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Wire.h>
#include <time.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASS = "";

#define BTN_NEXT 4
#define BTN_VIEW 5
#define BTN_PREV 18
#define NC 10
#define N 7
#define LIVE_MS (30UL * 1000UL)
#define HIST_MS (6UL * 60UL * 60UL * 1000UL)
#define AUTO_IDLE_MS (20UL * 1000UL)
#define AUTO_STEP_MS (4UL * 1000UL)
#define LIVE_FIAT 1
#define TZ_OFFSET_H 10

Adafruit_SSD1306 display(128, 64, &Wire, -1);
Preferences prefs;

struct Asset {
  const char* code;
  const char* geckoId;
  int nominal;
  float hist[N];
  int count;
  float live;
  float change24;
  bool hasChange;
  int8_t liveDir;
};

Asset assets[NC] = {
  {"USD", "tether", 1, {0}, 0, 0, 0, false},
  {"EUR", "euro-coin", 1, {0}, 0, 0, 0, false},
  {"CNY", nullptr, 1, {0}, 0, 0, 0, false},
  {"KZT", nullptr, 1, {0}, 0, 0, 0, false},
  {"JPY", nullptr, 1, {0}, 0, 0, 0, false},
  {"GBP", nullptr, 1, {0}, 0, 0, 0, false},
  {"TRY", nullptr, 1, {0}, 0, 0, 0, false},
  {"CHF", nullptr, 1, {0}, 0, 0, 0, false},
  {"BYN", nullptr, 1, {0}, 0, 0, 0, false},
  {"AED", nullptr, 1, {0}, 0, 0, 0, false},
};

int curIdx = 0;

enum ViewMode { VIEW_RATE, VIEW_CHART, VIEW_CONV };
enum Busy { BUSY_NONE, BUSY_LIVE, BUSY_HIST };

ViewMode viewMode = VIEW_RATE;
Busy busy = BUSY_NONE;

bool showStatus = false;
uint32_t lastStatusDraw = 0;
uint32_t liveOkMs = 0;
uint32_t histOkMs = 0;
bool histCached = false;

const long CONV_AMOUNTS[] = {1, 10, 50, 100, 500, 1000, 5000, 10000};
#define CONV_N ((int)(sizeof(CONV_AMOUNTS) / sizeof(CONV_AMOUNTS[0])))

int convAmtIdx = 3;
bool convRev = false;

void draw();
bool autoMode = false;

uint32_t lastLive = 0, lastHist = 0, lastInput = 0, lastAuto = 0;

String lastUpdate = "--:--";

char dateOld[8] = "";
char dateNew[8] = "";

String fmt(float v) {
  char buf[16];
  if (v < 1.0f) snprintf(buf, sizeof(buf), "%.4f", v);
  else snprintf(buf, sizeof(buf), "%.2f", v);
  return String(buf);
}

String fmtDec(float v, int dec) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%.*f", dec, v);
  return String(buf);
}

String fmtMoney(float v) {
  char buf[20];
  if (v >= 100000.0f) snprintf(buf, sizeof(buf), "%.0f", v);
  else if (v >= 1.0f) snprintf(buf, sizeof(buf), "%.2f", v);
  else snprintf(buf, sizeof(buf), "%.4f", v);
  return String(buf);
}

String agoStr(uint32_t ms) {
  unsigned long s = ms / 1000;
  char b[12];
  if (s < 60) snprintf(b, sizeof(b), "%lus", s);
  else if (s < 3600) snprintf(b, sizeof(b), "%lum", s / 60);
  else snprintf(b, sizeof(b), "%luh%02lum", s / 3600, (s % 3600) / 60);
  return String(b);
}

String clockStr() {
  if (time(nullptr) < 1700000000) return "--:--";
  struct tm t;
  if (!getLocalTime(&t, 50)) return "--:--";
  char b[8];
  strftime(b, sizeof(b), "%H:%M", &t);
  return String(b);
}

void showMessage(const char* l1, const char* l2 = "") {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 20);
  display.println(l1);
  display.println(l2);
  display.display();
}

void showProgress(const char* label, int cur, int total) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 18);
  display.print(label);
  display.drawRoundRect(0, 32, 128, 12, 3, SSD1306_WHITE);
  int w = 124 * cur / total;
  if (w > 0) display.fillRect(2, 34, w, 8, SSD1306_WHITE);
  char b[16];
  snprintf(b, sizeof(b), "%d / %d", cur, total);
  display.setCursor(0, 50);
  display.print(b);
  display.display();
}

void splash() {
  for (int f = 0; f < 24; f++) {
    display.clearDisplay();
    int rx = (int)(fabsf(cosf(f * 0.4f)) * 14.0f) + 2;
    for (int dy = -14; dy <= 14; dy++) {
      int half = (int)(rx * sqrtf(1.0f - (dy * dy) / (14.0f * 14.0f)));
      display.drawFastHLine(64 - half, 22 + dy, 2 * half + 1, SSD1306_WHITE);
    }
    if (rx > 8) {
      int k = (int)((f * 0.4f + 1.5708f) / 3.14159f);
      display.setTextColor(SSD1306_BLACK);
      display.setTextSize(2);
      display.setCursor(58, 15);
      display.print("$ER"[k % 3]);
    }
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(16, 46);
    display.print("Currency Tracker");
    display.display();
    delay(40);
  }
}

float currentValue(const Asset& a) {
  if (a.live > 0) return a.live;
  if (a.count > 0) return a.hist[a.count - 1];
  return 0;
}

#define CACHE_MAGIC (0xC0DE0000UL | ((uint32_t)NC << 8) | (uint32_t)N)

struct Cache {
  uint32_t magic;
  uint32_t epoch;
  int count;
  int nominal[NC];
  float hist[NC][N];
  char dOld[8];
  char dNew[8];
  char stamp[8];
};

void saveCache() {
  static Cache c;
  memset(&c, 0, sizeof(c));
  c.magic = CACHE_MAGIC;
  time_t now = time(nullptr);
  c.epoch = now > 1700000000 ? (uint32_t)now : 0;
  c.count = assets[0].count;

  for (int i = 0; i < NC; i++) {
    c.nominal[i] = assets[i].nominal;
    for (int k = 0; k < N; k++) c.hist[i][k] = assets[i].hist[k];
  }

  strlcpy(c.dOld, dateOld, sizeof(c.dOld));
  strlcpy(c.dNew, dateNew, sizeof(c.dNew));
  strlcpy(c.stamp, lastUpdate.c_str(), sizeof(c.stamp));

  if (prefs.begin("rates", false)) {
    prefs.putBytes("c", &c, sizeof(c));
    prefs.end();
  }
}

bool loadCache(uint32_t& epoch) {
  static Cache c;
  if (!prefs.begin("rates", true)) return false;

  size_t n = prefs.getBytes("c", &c, sizeof(c));
  prefs.end();

  if (n != sizeof(c) || c.magic != CACHE_MAGIC || c.count < 2 || c.count > N)
    return false;

  for (int i = 0; i < NC; i++) {
    assets[i].nominal = c.nominal[i] > 0 ? c.nominal[i] : 1;
    assets[i].count = c.count;
    for (int k = 0; k < N; k++) assets[i].hist[k] = c.hist[i][k];
  }

  strlcpy(dateOld, c.dOld, sizeof(dateOld));
  strlcpy(dateNew, c.dNew, sizeof(dateNew));
  lastUpdate = String(c.stamp);
  epoch = c.epoch;

  return true;
}

bool fetchCbr() {
  JsonDocument filter;
  filter["PreviousURL"] = true;
  filter["Date"] = true;

  for (int i = 0; i < NC; i++) {
    filter["Valute"][assets[i].code]["Value"] = true;
    filter["Valute"][assets[i].code]["Nominal"] = true;
  }

  static float vals[NC][N];
  int nom[NC];

  for (int i = 0; i < NC; i++) nom[i] = 1;

  int got = 0;
  String url = "https://www.cbr-xml-daily.ru/daily_json.js";

  while (got < N && url.length() > 0) {
    showProgress("Loading history...", got, N);

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    http.useHTTP10(true);
    http.setTimeout(10000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    if (!http.begin(client, url)) break;

    int code = http.GET();

    if (code != 200) {
      Serial.printf("CBR http error %d\n", code);
      http.end();
      break;
    }

    JsonDocument doc;
    DeserializationError err =
        deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));

    http.end();

    if (err) {
      Serial.printf("CBR json error: %s\n", err.c_str());
      break;
    }

    for (int i = 0; i < NC; i++) {
      JsonObject v = doc["Valute"][assets[i].code];
      float val = v["Value"] | 0.0f;
      int n = v["Nominal"] | 1;

      if (n > 0) nom[i] = n;
      if (val <= 0 && got > 0) val = vals[i][got - 1];

      vals[i][got] = val;
    }

    const char* d = doc["Date"];

    if (d && strlen(d) >= 10) {
      char tmp[8];
      snprintf(tmp, sizeof(tmp), "%.2s.%.2s", d + 8, d + 5);
      if (got == 0) strlcpy(dateNew, tmp, sizeof(dateNew));
      strlcpy(dateOld, tmp, sizeof(dateOld));
    }

    got++;

    const char* prev = doc["PreviousURL"];
    url = prev ? (String("https:") + prev) : String("");
  }

  if (got == 0) return false;

  showProgress("Loading history...", got, N);

  for (int i = 0; i < NC; i++) {
    assets[i].nominal = nom[i];
    assets[i].count = got;
    for (int k = 0; k < got; k++)
      assets[i].hist[k] = vals[i][got - 1 - k];
  }

  Serial.printf("CBR history ok, days: %d\n", got);
  return true;
}

bool fetchLive() {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.useHTTP10(true);
  http.setTimeout(10000);

  if (!http.begin(client,
      "https://api.coingecko.com/api/v3/simple/price?ids=tether,euro-coin"
      "&vs_currencies=rub&include_24hr_change=true"))
    return false;

  http.addHeader("User-Agent", "esp32-currency-tracker");

  int code = http.GET();

  if (code != 200) {
    Serial.printf("Live http error %d\n", code);
    http.end();
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();

  if (err) {
    Serial.printf("Live json error: %s\n", err.c_str());
    return false;
  }

  int updated = 0;

  for (int i = 0; i < NC; i++) {
    if (!assets[i].geckoId) continue;

    JsonObject o = doc[assets[i].geckoId];
    float v = o["rub"] | 0.0f;

    if (v <= 0) continue;

    float nv = v * assets[i].nominal;

    if (assets[i].live > 0) {
      float d = nv - assets[i].live;
      if (d > 0.0001f) assets[i].liveDir = 1;
      else if (d < -0.0001f) assets[i].liveDir = -1;
    }

    assets[i].live = nv;

    if (!o["rub_24h_change"].isNull()) {
      assets[i].change24 = o["rub_24h_change"].as<float>();
      assets[i].hasChange = true;
    }

    updated++;
  }

  Serial.printf("Live ok, updated: %d\n", updated);
  return updated > 0;
}

void connectWifi(bool ui = true) {
  if (ui) showMessage("Connecting WiFi...");

  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 15000)
    delay(200);

  if (WiFi.status() == WL_CONNECTED) {
    configTime(TZ_OFFSET_H * 3600, 0, "pool.ntp.org", "time.google.com");

    t = millis();
    while (time(nullptr) < 1700000000 && millis() - t < 4000)
      delay(100);
  }
}

bool ensureWifi(bool quiet = false) {
  if (WiFi.status() == WL_CONNECTED) return true;
  if (quiet) return false;

  connectWifi();

  if (WiFi.status() != WL_CONNECTED) {
    showMessage("No WiFi");
    delay(1500);
    return false;
  }

  return true;
}

void refreshHistory() {
  lastHist = millis();

  if (!ensureWifi()) return;

  busy = BUSY_HIST;
  bool ok = fetchCbr();
  busy = BUSY_NONE;

  if (ok) {
    lastUpdate = clockStr();
    histOkMs = millis();
    histCached = false;
    saveCache();
  } else {
    showMessage("History failed", "using last data");
    delay(1500);
  }
}

bool refreshLive(bool quiet = false) {
  lastLive = millis();

  if (!LIVE_FIAT) return false;
  if (!ensureWifi(quiet)) return false;

  if (!quiet) showMessage("Updating rates...");

  busy = BUSY_LIVE;

  if (quiet && showStatus) draw();

  bool ok = fetchLive();
  busy = BUSY_NONE;

  if (ok) {
    lastUpdate = clockStr();
    liveOkMs = millis();
    return true;
  }

  if (!quiet) {
    showMessage("Live update failed", "using CBR rates");
    delay(1200);
  }

  return false;
}

void drawRate() {
  Asset& a = assets[curIdx];

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  float now = currentValue(a);

  if (now <= 0) {
    showMessage("No data", "wait for update...");
    return;
  }

  display.setTextSize(1);
  display.setCursor(0, 0);

  if (a.nominal > 1) display.printf("%d %s =", a.nominal, a.code);
  else display.printf("1 %s =", a.code);

  display.setCursor(62, 0);
  display.print(lastUpdate);

  char idx[8];
  snprintf(idx, sizeof(idx), "%d/%d", curIdx + 1, NC);

  display.setCursor(128 - strlen(idx) * 6, 0);
  display.print(idx);

  display.setTextSize(2);
  display.setCursor(0, 16);

  String valStr = fmt(now);
  display.print(valStr);

  if (a.live > 0 && a.liveDir != 0) {
    int ax = valStr.length() * 12 + 4;

    if (ax + 8 < 128) {
      if (a.liveDir > 0)
        display.fillTriangle(ax, 28, ax + 8, 28, ax + 4, 18, SSD1306_WHITE);
      else
        display.fillTriangle(ax, 18, ax + 8, 18, ax + 4, 28, SSD1306_WHITE);
    }
  }

  display.setTextSize(1);
  display.setCursor(0, 34);
  display.print("RUB");

  display.setCursor(98, 34);
  display.print(a.live > 0 ? "live" : "CBR");

  char line[24];
  line[0] = 0;

  float chg = 0;

  if (a.live > 0 && a.hasChange) {
    chg = a.change24;
    snprintf(line, sizeof(line), "%+.2f%% 24h", chg);
  } else if (a.count >= 2) {
    float prev = a.hist[a.count - 2];
    float d = a.hist[a.count - 1] - prev;
    float p = prev != 0 ? d / prev * 100.0f : 0;

    chg = d;
    snprintf(line, sizeof(line), "%+.2f (%+.2f%%) 1d", d, p);
  }

  if (line[0]) {
    if (chg > 0)
      display.fillTriangle(0, 57, 6, 57, 3, 50, SSD1306_WHITE);
    else if (chg < 0)
      display.fillTriangle(0, 50, 6, 50, 3, 57, SSD1306_WHITE);

    display.setCursor(10, 50);
    display.print(line);
  }

  if (autoMode) display.fillCircle(124, 55, 2, SSD1306_WHITE);
}

void drawChart() {
  Asset& a = assets[curIdx];

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  if (a.count < 2) {
    showMessage("No history yet", "wait for update...");
    return;
  }

  float now = a.hist[a.count - 1];
  float prev = a.hist[a.count - 2];
  float pct = prev != 0 ? (now - prev) / prev * 100.0f : 0;

  String title = (a.nominal > 1 ? String(a.nominal) : String("")) + a.code;

  display.setCursor(0, 0);
  display.print(title);

  display.setCursor(title.length() * 6 + 6, 0);
  display.printf("%+.2f%%", pct);

  String s = fmt(now);
  display.setCursor(128 - s.length() * 6, 0);
  display.print(s);

  float mn = a.hist[0], mx = a.hist[0];

  for (int i = 1; i < a.count; i++) {
    mn = min(mn, a.hist[i]);
    mx = max(mx, a.hist[i]);
  }

  float lo, hi;

  if (mx - mn < mx * 0.0005f) {
    lo = mn * 0.998f;
    hi = mx * 1.002f;
  } else {
    float pad = (mx - mn) * 0.12f;
    lo = mn - pad;
    hi = mx + pad;
  }

  float step = (hi - lo) / 2.0f;
  int dec = step >= 2 ? 0 : step >= 0.2f ? 1 : step >= 0.02f ? 2 : 3;

  const int xL = 40, xR = 126, yT = 14, yB = 51;

  display.drawFastVLine(38, yT - 2, yB - yT + 5, SSD1306_WHITE);
  display.drawFastHLine(38, yB + 2, xR - 38 + 1, SSD1306_WHITE);

  for (int k = 0; k < 3; k++) {
    float val = hi - step * k;
    int y = yT + (yB - yT) * k / 2;

    String lab = fmtDec(val, dec);
    display.setCursor(0, y - 3);
    display.print(lab);

    for (int x = xL; x <= xR; x += 4)
      display.drawPixel(x, y, SSD1306_WHITE);
  }

  int px = 0, py = 0;

  for (int i = 0; i < a.count; i++) {
    int x = xL + (xR - xL) * i / (a.count - 1);
    int y = yB - (int)((a.hist[i] - lo) / (hi - lo) * (yB - yT));

    if (i > 0) display.drawLine(px, py, x, y, SSD1306_WHITE);

    display.drawFastVLine(x, yB + 3, 2, SSD1306_WHITE);

    if (i == a.count - 1)
      display.fillCircle(x, y, 2, SSD1306_WHITE);
    else
      display.fillCircle(x, y, 1, SSD1306_WHITE);

    px = x;
    py = y;
  }

  if (dateOld[0]) {
    display.setCursor(xL - 2, 57);
    display.print(dateOld);

    display.setCursor(128 - strlen(dateNew) * 6, 57);
    display.print(dateNew);
  }
}

void drawConverter() {
  Asset& a = assets[curIdx];

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  float cur = currentValue(a);

  if (cur <= 0 || a.nominal <= 0) {
    showMessage("No data", "wait for update...");
    return;
  }

  float perUnit = cur / a.nominal;
  long amt = CONV_AMOUNTS[convAmtIdx];

  display.setCursor(0, 0);
  display.print("CONVERT");

  char idx[8];
  snprintf(idx, sizeof(idx), "%d/%d", curIdx + 1, NC);

  display.setCursor(128 - strlen(idx) * 6, 0);
  display.print(idx);

  const char* from = convRev ? "RUB" : a.code;
  const char* to = convRev ? a.code : "RUB";

  float res = convRev ? amt / perUnit : amt * perUnit;

  display.setCursor(0, 12);
  display.printf("%ld %s =", amt, from);

  display.setTextSize(2);
  display.setCursor(0, 24);
  display.print(fmtMoney(res));

  display.setTextSize(1);
  display.setCursor(0, 44);
  display.print(to);

  String r = "@" + fmt(perUnit);
  display.setCursor(128 - r.length() * 6, 44);
  display.print(r);

  display.setCursor(0, 56);
  display.print("hold>:sum  hold<:flip");
}

const char* modeName() {
  switch (viewMode) {
    case VIEW_CHART: return "CHART";
    case VIEW_CONV: return "CONV";
    default: return "RATE";
  }
}

void drawStatus() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  display.setCursor(0, 0);
  display.print("STATUS");

  bool wifiOk = WiFi.status() == WL_CONNECTED;
  int rssi = wifiOk ? WiFi.RSSI() : -127;
  int level = !wifiOk ? 0 : rssi > -55 ? 4 : rssi > -65 ? 3 : rssi > -75 ? 2 : 1;

  for (int b = 0; b < 4; b++) {
    int h = 2 + b * 2;

    if (b < level)
      display.fillRect(58 + b * 4, 8 - h, 3, h, SSD1306_WHITE);
    else
      display.drawPixel(59 + b * 4, 7, SSD1306_WHITE);
  }

  String clk = clockStr();
  display.setCursor(128 - clk.length() * 6, 0);
  display.print(clk);

  display.drawFastHLine(0, 10, 128, SSD1306_WHITE);

  display.setCursor(0, 14);

  if (wifiOk) display.printf("WiFi: OK %ddBm", rssi);
  else display.print("WiFi: no link");

  display.setCursor(0, 24);

  if (busy == BUSY_LIVE) {
    display.print("Data: updating live");
  } else if (busy == BUSY_HIST) {
    display.print("Data: loading hist");
  } else if (LIVE_FIAT) {
    long left = ((long)LIVE_MS - (long)(millis() - lastLive)) / 1000;
    if (left < 0) left = 0;
    display.printf("Data: idle  next %lds", left);
  } else {
    display.print("Data: idle");
  }

  if (busy != BUSY_NONE) display.fillCircle(121, 27, 3, SSD1306_WHITE);

  display.setCursor(0, 34);

  if (liveOkMs)
    display.printf("Live: %s ago", agoStr(millis() - liveOkMs).c_str());
  else
    display.print("Live: --");

  display.setCursor(0, 44);

  if (histOkMs)
    display.printf("Hist: %s ago", agoStr(millis() - histOkMs).c_str());
  else
    display.print(histCached ? "Hist: cached" : "Hist: --");

  display.setCursor(0, 54);
  display.printf("Mode: %s %s %d/%d",
                 modeName(), assets[curIdx].code, curIdx + 1, NC);
}

void render() {
  if (showStatus) {
    drawStatus();
    return;
  }

  switch (viewMode) {
    case VIEW_CHART: drawChart(); break;
    case VIEW_CONV: drawConverter(); break;
    default: drawRate(); break;
  }
}

void draw() {
  render();
  display.display();
}

static uint8_t oldBuf[1024], newBuf[1024];

void drawAnimated(int dir) {
  uint8_t* b = display.getBuffer();
  memcpy(oldBuf, b, 1024);

  render();
  memcpy(newBuf, b, 1024);

  const int offs[] = {20, 48, 80, 108, 128};

  for (int k = 0; k < 5; k++) {
    int s = offs[k];

    for (int p = 0; p < 8; p++) {
      for (int x = 0; x < 128; x++) {
        uint8_t v;

        if (dir > 0) {
          int sx = x + s;
          v = sx < 128 ? oldBuf[p * 128 + sx] : newBuf[p * 128 + sx - 128];
        } else {
          int sx = x - s;
          v = sx >= 0 ? oldBuf[p * 128 + sx] : newBuf[p * 128 + sx + 128];
        }

        b[p * 128 + x] = v;
      }
    }

    display.display();
  }

  memcpy(b, newBuf, 1024);
  display.display();
}

#define LONG_MS 600

enum BtnEvent { EV_NONE, EV_SHORT, EV_LONG };

struct Btn {
  int pin;
  bool last;
  uint32_t t;
  uint32_t downAt;
  bool longFired;
};

Btn bNext = {BTN_NEXT, HIGH, 0, 0, false};
Btn bView = {BTN_VIEW, HIGH, 0, 0, false};
Btn bPrev = {BTN_PREV, HIGH, 0, 0, false};

BtnEvent pollBtn(Btn& b) {
  bool state = digitalRead(b.pin);
  uint32_t now = millis();

  if (state != b.last && now - b.t > 40) {
    b.t = now;
    b.last = state;

    if (state == LOW) {
      b.downAt = now;
      b.longFired = false;
    } else if (!b.longFired) {
      return EV_SHORT;
    }
  }

  if (b.last == LOW && !b.longFired && now - b.downAt > LONG_MS) {
    b.longFired = true;
    return EV_LONG;
  }

  return EV_NONE;
}

void setup() {
  Serial.begin(115200);

  pinMode(BTN_NEXT, INPUT_PULLUP);
  pinMode(BTN_VIEW, INPUT_PULLUP);
  pinMode(BTN_PREV, INPUT_PULLUP);

  Wire.begin(21, 22);
  Wire.setClock(400000);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED not found");
    while (true) delay(1000);
  }

  display.clearDisplay();
  splash();

  uint32_t cacheEpoch = 0;
  bool haveCache = loadCache(cacheEpoch);

  if (haveCache) draw();

  connectWifi(!haveCache);

  time_t now = time(nullptr);

  bool fresh = haveCache && cacheEpoch > 0 && now > 1700000000 &&
               (uint32_t)now - cacheEpoch < HIST_MS / 1000;

  if (fresh) {
    lastHist = millis();
    histCached = true;
  } else {
    refreshHistory();
  }

  refreshLive();

  lastInput = millis();
  draw();
}

void loop() {
  BtnEvent en = pollBtn(bNext);
  BtnEvent ep = pollBtn(bPrev);
  BtnEvent ev = pollBtn(bView);

  bool any = (en != EV_NONE || ep != EV_NONE || ev != EV_NONE);

  if (any) {
    lastInput = millis();
    autoMode = false;
  }

  if (any && showStatus) {
    showStatus = false;
    drawAnimated(-1);
  } else if (ev == EV_LONG) {
    showStatus = true;
    drawAnimated(+1);
  } else if (ev == EV_SHORT) {
    viewMode = (ViewMode)((viewMode + 1) % 3);
    drawAnimated(+1);
  } else if (en == EV_LONG && viewMode == VIEW_CONV) {
    convAmtIdx = (convAmtIdx + 1) % CONV_N;
    draw();
  } else if (ep == EV_LONG && viewMode == VIEW_CONV) {
    convRev = !convRev;
    draw();
  } else if (en != EV_NONE) {
    curIdx = (curIdx + 1) % NC;
    drawAnimated(+1);
  } else if (ep != EV_NONE) {
    curIdx = (curIdx + NC - 1) % NC;
    drawAnimated(-1);
  }

  if (!autoMode && !showStatus && viewMode != VIEW_CONV &&
      millis() - lastInput > AUTO_IDLE_MS) {
    autoMode = true;
    lastAuto = millis();
    draw();
  }

  if (autoMode && millis() - lastAuto > AUTO_STEP_MS) {
    if (viewMode == VIEW_RATE) {
      viewMode = VIEW_CHART;
    } else {
      viewMode = VIEW_RATE;
      curIdx = (curIdx + 1) % NC;
    }

    drawAnimated(+1);
    lastAuto = millis();
  }

  if (millis() - lastLive > LIVE_MS) {
    bool ok = refreshLive(true);
    if (ok || showStatus) draw();
  }

  if (millis() - lastHist > HIST_MS) {
    refreshHistory();
    draw();
  }

  if (showStatus && millis() - lastStatusDraw >= 1000) {
    lastStatusDraw = millis();
    draw();
  }
}

