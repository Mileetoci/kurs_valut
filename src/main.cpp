#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Wire.h>
#include <time.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// Архитектура: loop() (ядро 1) занимается только экраном и кнопками.
// Вся сеть (Wi-Fi, NTP, HTTPS-запросы) живёт в отдельной задаче netTask (ядро 0).
// Сетевая задача ничего не рисует и не трогает общие данные, пока качает:
// сначала всё складывает в локальные буферы, а в общие данные пишет
// коротко, под мьютексом, когда загрузка уже закончена.

const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASS = "";

// большие кнопки
#define BTN_NEXT 4       // следующая валюта
#define BTN_PREV 18      // предыдущая валюта
#define BTN_VIEW 5       // режим: курс -> график -> конвертер
// маленькие кнопки
#define BTN_UP 19        // сумма в конвертере больше
#define BTN_DOWN 23      // сумма в конвертере меньше
#define BTN_STATUS 25    // экран статуса
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
enum Busy { BUSY_NONE, BUSY_WIFI, BUSY_LIVE, BUSY_HIST };

ViewMode viewMode = VIEW_RATE;

// --- общее состояние между экраном и сетевой задачей ---
SemaphoreHandle_t dataMux;                 // защищает assets, dateOld/New, lastUpdate, *OkMs
#define LOCK()   xSemaphoreTake(dataMux, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(dataMux)

volatile Busy busy = BUSY_NONE;            // что сейчас делает сеть
volatile bool dataDirty = false;           // сеть просит перерисовать экран
volatile bool liveFail = false;
volatile bool histFail = false;
volatile uint8_t liveSrc = 0;              // откуда последний живой курс: 1 = CoinGecko, 2 = Binance
volatile int histProgress = 0;             // сколько дней истории уже загружено
volatile uint32_t lastLive = 0;            // когда сеть начала последнее живое обновление

bool haveCache = false;                    // выставляется в setup() до старта сети
uint32_t cacheEpoch = 0;

bool dotShown = false;                     // мигающая точка "идёт загрузка"

bool showStatus = false;
uint32_t lastStatusDraw = 0;
uint32_t liveOkMs = 0;
uint32_t histOkMs = 0;
bool histCached = false;

const long CONV_AMOUNTS[] = {1, 10, 50, 100, 500, 1000, 5000, 10000};
#define CONV_N ((int)(sizeof(CONV_AMOUNTS) / sizeof(CONV_AMOUNTS[0])))

int convAmtIdx = 3;

void draw();
bool autoMode = false;

uint32_t lastInput = 0, lastAuto = 0;

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

// Рисовать на дисплее можно только из loop()/setup(), сетевая задача его не трогает.
void showMessage(const char* l1, const char* l2 = "") {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 20);
  display.println(l1);
  display.println(l2);
  display.display();
}

// экран "данных пока нет" (первый запуск без кэша)
void noData() {
  char l2[24];
  if (busy == BUSY_HIST) snprintf(l2, sizeof(l2), "loading %d/%d", (int)histProgress, N);
  else if (WiFi.status() != WL_CONNECTED) snprintf(l2, sizeof(l2), "connecting WiFi...");
  else snprintf(l2, sizeof(l2), "wait for update...");
  showMessage("No data", l2);
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

// вызывается из сетевой задачи: снимок данных копируется под мьютексом,
// а медленная запись во флеш идёт уже без него
void saveCache() {
  static Cache c;
  memset(&c, 0, sizeof(c));
  c.magic = CACHE_MAGIC;
  time_t now = time(nullptr);
  c.epoch = now > 1700000000 ? (uint32_t)now : 0;

  LOCK();
  c.count = assets[0].count;

  for (int i = 0; i < NC; i++) {
    c.nominal[i] = assets[i].nominal;
    for (int k = 0; k < N; k++) c.hist[i][k] = assets[i].hist[k];
  }

  strlcpy(c.dOld, dateOld, sizeof(c.dOld));
  strlcpy(c.dNew, dateNew, sizeof(c.dNew));
  strlcpy(c.stamp, lastUpdate.c_str(), sizeof(c.stamp));
  UNLOCK();

  if (prefs.begin("rates", false)) {
    prefs.putBytes("c", &c, sizeof(c));
    prefs.end();
  }
}

// вызывается только из setup(), до старта сетевой задачи
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

// ================= СЕТЬ (работает в netTask) =================

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
  char dNewL[8] = "", dOldL[8] = "";

  for (int i = 0; i < NC; i++) nom[i] = 1;

  int got = 0;
  String url = "https://www.cbr-xml-daily.ru/daily_json.js";

  while (got < N && url.length() > 0) {
    histProgress = got;
    dataDirty = true;

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
      if (got == 0) strlcpy(dNewL, tmp, sizeof(dNewL));
      strlcpy(dOldL, tmp, sizeof(dOldL));
    }

    got++;
    vTaskDelay(pdMS_TO_TICKS(20));   // отдаём время системным задачам между запросами

    const char* prev = doc["PreviousURL"];
    url = prev ? (String("https:") + prev) : String("");
  }

  if (got == 0) return false;

  histProgress = got;

  // единственное место, где история попадает в общие данные
  LOCK();
  for (int i = 0; i < NC; i++) {
    assets[i].nominal = nom[i];
    assets[i].count = got;
    for (int k = 0; k < got; k++)
      assets[i].hist[k] = vals[i][got - 1 - k];
  }
  strlcpy(dateNew, dNewL, sizeof(dateNew));
  strlcpy(dateOld, dOldL, sizeof(dateOld));
  UNLOCK();

  Serial.printf("CBR history ok, days: %d\n", got);
  return true;
}

// Источник 1 (приоритетный): CoinGecko. Заполняет nv/ch/hc, в общие данные не пишет.
bool fetchLiveGecko(float nv[], float ch[], bool hc[]) {
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

  int any = 0;

  for (int i = 0; i < NC; i++) {
    if (!assets[i].geckoId) continue;

    JsonObject o = doc[assets[i].geckoId];
    float v = o["rub"] | 0.0f;

    if (v <= 0) continue;

    nv[i] = v * assets[i].nominal;
    any++;

    if (!o["rub_24h_change"].isNull()) {
      ch[i] = o["rub_24h_change"].as<float>();
      hc[i] = true;
    }
  }

  return any > 0;
}

// Источник 2 (запасной): Binance. USD = USDT/RUB, EUR = EUR/USDT * USDT/RUB.
bool fetchLiveBinance(float nv[], float ch[], bool hc[]) {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.useHTTP10(true);
  http.setTimeout(10000);

  if (!http.begin(client,
      "https://data-api.binance.vision/api/v3/ticker/24hr"
      "?symbols=%5B%22USDTRUB%22,%22EURUSDT%22%5D"))
    return false;

  http.addHeader("User-Agent", "esp32-currency-tracker");

  int code = http.GET();

  if (code != 200) {
    Serial.printf("Binance http error %d\n", code);
    http.end();
    return false;
  }

  JsonDocument filter;
  filter[0]["symbol"] = true;
  filter[0]["lastPrice"] = true;
  filter[0]["priceChangePercent"] = true;

  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();

  if (err) {
    Serial.printf("Binance json error: %s\n", err.c_str());
    return false;
  }

  float usdRub = 0, usdPct = 0, eurUsd = 0, eurPct = 0;

  for (JsonObject o : doc.as<JsonArray>()) {
    const char* sym = o["symbol"] | "";
    float price = atof(o["lastPrice"] | "0");
    float pct = atof(o["priceChangePercent"] | "0");

    if (!strcmp(sym, "USDTRUB")) { usdRub = price; usdPct = pct; }
    else if (!strcmp(sym, "EURUSDT")) { eurUsd = price; eurPct = pct; }
  }

  if (usdRub <= 0) return false;

  int any = 0;

  for (int i = 0; i < NC; i++) {
    if (!assets[i].geckoId) continue;

    if (!strcmp(assets[i].code, "USD")) {
      nv[i] = usdRub * assets[i].nominal;
      ch[i] = usdPct;
      hc[i] = true;
      any++;
    } else if (!strcmp(assets[i].code, "EUR") && eurUsd > 0) {
      nv[i] = eurUsd * usdRub * assets[i].nominal;
      ch[i] = ((1.0f + eurPct / 100.0f) * (1.0f + usdPct / 100.0f) - 1.0f) * 100.0f;
      hc[i] = true;
      any++;
    }
  }

  return any > 0;
}

// Живой курс: сначала CoinGecko, если не ответил — Binance.
bool fetchLive() {
  float nv[NC];
  float ch[NC];
  bool hc[NC];

  for (int i = 0; i < NC; i++) {
    nv[i] = 0;
    ch[i] = 0;
    hc[i] = false;
  }

  uint8_t src = 0;

  if (fetchLiveGecko(nv, ch, hc)) src = 1;
  else if (fetchLiveBinance(nv, ch, hc)) src = 2;
  else return false;

  int updated = 0;

  // единственное место, где живой курс попадает в общие данные
  LOCK();
  for (int i = 0; i < NC; i++) {
    if (nv[i] <= 0) continue;

    if (assets[i].live > 0) {
      float d = nv[i] - assets[i].live;
      if (d > 0.0001f) assets[i].liveDir = 1;
      else if (d < -0.0001f) assets[i].liveDir = -1;
    }

    assets[i].live = nv[i];

    if (hc[i]) {
      assets[i].change24 = ch[i];
      assets[i].hasChange = true;
    }

    updated++;
  }
  UNLOCK();

  liveSrc = src;
  Serial.printf("Live ok (%s), updated: %d\n", src == 1 ? "CoinGecko" : "Binance", updated);
  return updated > 0;
}

// подключение к Wi-Fi и синхронизация времени; ждёт через vTaskDelay,
// поэтому на экран и кнопки это никак не влияет
bool netWifiUp() {
  if (WiFi.status() == WL_CONNECTED) return true;

  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 15000)
    vTaskDelay(pdMS_TO_TICKS(200));

  if (WiFi.status() != WL_CONNECTED) return false;

  if (time(nullptr) < 1700000000) {
    configTime(TZ_OFFSET_H * 3600, 0, "pool.ntp.org", "time.google.com");

    t = millis();
    while (time(nullptr) < 1700000000 && millis() - t < 4000)
      vTaskDelay(pdMS_TO_TICKS(100));
  }

  return true;
}

bool doHistory() {
  busy = BUSY_HIST;
  histProgress = 0;
  dataDirty = true;

  bool ok = fetchCbr();

  if (ok) {
    String stamp = clockStr();

    LOCK();
    lastUpdate = stamp;
    histOkMs = millis();
    histCached = false;
    UNLOCK();

    histFail = false;
    saveCache();
  } else {
    histFail = true;
  }

  busy = BUSY_NONE;
  dataDirty = true;
  return ok;
}

bool doLive() {
  lastLive = millis();

  if (!LIVE_FIAT) return false;

  busy = BUSY_LIVE;
  dataDirty = true;

  bool ok = fetchLive();

  if (ok) {
    String stamp = clockStr();

    LOCK();
    lastUpdate = stamp;
    liveOkMs = millis();
    UNLOCK();

    liveFail = false;
  } else {
    liveFail = true;
  }

  busy = BUSY_NONE;
  dataDirty = true;
  return ok;
}

void netTask(void*) {
  bool first = true;
  bool needHist = true;
  bool needLive = true;
  uint32_t lastHistT = 0;

  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      busy = BUSY_WIFI;
      dataDirty = true;

      bool ok = netWifiUp();

      busy = BUSY_NONE;
      dataDirty = true;

      if (!ok) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        continue;
      }
    }

    // кэш свежий — историю при старте перекачивать не нужно
    if (first) {
      first = false;

      time_t now = time(nullptr);
      bool fresh = haveCache && cacheEpoch > 0 && now > 1700000000 &&
                   (uint32_t)now - cacheEpoch < HIST_MS / 1000;

      if (fresh) {
        needHist = false;
        lastHistT = millis();

        LOCK();
        histCached = true;
        UNLOCK();
      }
    }

    if (needHist || millis() - lastHistT > HIST_MS) {
      needHist = false;
      lastHistT = millis();

      // не получилось — повторим через 30 секунд, а не через 6 часов
      if (!doHistory()) lastHistT = millis() - HIST_MS + 30000UL;
    }

    if (needLive || millis() - lastLive > LIVE_MS) {
      needLive = false;
      doLive();
    }

    vTaskDelay(pdMS_TO_TICKS(250));
  }
}

// ================= ЭКРАНЫ (только из loop()) =================

void drawRate() {
  Asset& a = assets[curIdx];

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  float now = currentValue(a);

  if (now <= 0) {
    noData();
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
    noData();
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
    noData();
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

  float res = amt * perUnit;            // amt единиц валюты -> рубли

  display.setCursor(0, 10);
  display.printf("%ld %s =", amt, a.code);

  display.setTextSize(2);
  display.setCursor(0, 20);
  display.print(fmtMoney(res));

  display.setTextSize(1);
  display.setCursor(0, 38);
  display.print("RUB");

  String r = "@" + fmt(perUnit);
  display.setCursor(128 - r.length() * 6, 38);
  display.print(r);

  // обратное направление всегда на экране: сколько валюты за amt рублей
  float back = amt / perUnit;
  char line[32];
  snprintf(line, sizeof(line), "%ld RUB=%s %s", amt, fmtMoney(back).c_str(), a.code);

  if (strlen(line) > 21)
    snprintf(line, sizeof(line), "%ld RUB=%.0f %s", amt, back, a.code);

  display.drawFastHLine(0, 48, 128, SSD1306_WHITE);
  display.setCursor(0, 53);
  display.print(line);
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

  Busy b = busy;

  if (b == BUSY_WIFI) {
    display.print("Data: connecting");
  } else if (b == BUSY_LIVE) {
    display.print("Data: updating live");
  } else if (b == BUSY_HIST) {
    display.print("Data: loading hist");
  } else if (LIVE_FIAT) {
    long left = ((long)LIVE_MS - (long)(millis() - lastLive)) / 1000;
    if (left < 0) left = 0;
    display.printf("Data: idle  next %lds", left);
  } else {
    display.print("Data: idle");
  }

  if (b != BUSY_NONE) display.fillCircle(121, 27, 3, SSD1306_WHITE);

  display.setCursor(0, 34);

  if (liveOkMs)
    display.printf("Live: %s ago%s%s", agoStr(millis() - liveOkMs).c_str(),
                   liveSrc == 1 ? " CG" : liveSrc == 2 ? " BN" : "",
                   liveFail ? " !" : "");
  else
    display.print(liveFail ? "Live: -- fail" : "Live: --");

  display.setCursor(0, 44);

  if (histOkMs)
    display.printf("Hist: %s ago%s", agoStr(millis() - histOkMs).c_str(),
                   histFail ? " !" : "");
  else if (histCached)
    display.print("Hist: cached");
  else
    display.print(histFail ? "Hist: -- fail" : "Hist: --");

  display.setCursor(0, 54);
  display.printf("Mode: %s %s %d/%d",
                 modeName(), assets[curIdx].code, curIdx + 1, NC);
}

// render() читает общие данные, поэтому идёт под мьютексом;
// сетевая задача держит его лишь на время короткой записи результата
void render() {
  LOCK();

  if (showStatus) {
    drawStatus();
    UNLOCK();
    return;
  }

  switch (viewMode) {
    case VIEW_CHART: drawChart(); break;
    case VIEW_CONV: drawConverter(); break;
    default: drawRate(); break;
  }

  // мигающая точка в правом верхнем углу: в фоне идёт загрузка
  if (dotShown) display.fillCircle(125, 11, 2, SSD1306_WHITE);

  UNLOCK();
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

struct Btn {
  int pin;
  bool last;
  uint32_t t;
};

Btn bNext = {BTN_NEXT, HIGH, 0};
Btn bPrev = {BTN_PREV, HIGH, 0};
Btn bView = {BTN_VIEW, HIGH, 0};
Btn bUp = {BTN_UP, HIGH, 0};
Btn bDown = {BTN_DOWN, HIGH, 0};
Btn bStatus = {BTN_STATUS, HIGH, 0};

// true один раз в момент нажатия (с защитой от дребезга)
bool pollBtn(Btn& b) {
  bool state = digitalRead(b.pin);
  uint32_t now = millis();

  if (state != b.last && now - b.t > 40) {
    b.t = now;
    b.last = state;

    if (state == LOW) return true;
  }

  return false;
}

void setup() {
  Serial.begin(115200);

  pinMode(BTN_NEXT, INPUT_PULLUP);
  pinMode(BTN_VIEW, INPUT_PULLUP);
  pinMode(BTN_PREV, INPUT_PULLUP);
  pinMode(BTN_UP, INPUT_PULLUP);
  pinMode(BTN_DOWN, INPUT_PULLUP);
  pinMode(BTN_STATUS, INPUT_PULLUP);

  Wire.begin(21, 22);
  Wire.setClock(400000);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED not found");
    while (true) delay(1000);
  }

  display.clearDisplay();

  dataMux = xSemaphoreCreateMutex();

  // кэш читаем до старта сети, чтобы никто не писал в данные одновременно
  haveCache = loadCache(cacheEpoch);

  // сеть стартует в фоне на ядре 0 (loop() работает на ядре 1)
  // приоритет 0 (как у IDLE0), иначе тяжёлый TLS не даёт idle-задаче сбросить watchdog
  xTaskCreatePinnedToCore(netTask, "net", 20480, nullptr, 0, nullptr, 0);

  splash();

  lastInput = millis();
  draw();
}

void loop() {
  bool en = pollBtn(bNext);
  bool ep = pollBtn(bPrev);
  bool ev = pollBtn(bView);
  bool eu = pollBtn(bUp);
  bool ed = pollBtn(bDown);
  bool es = pollBtn(bStatus);

  bool any = en || ep || ev || eu || ed || es;

  if (any) {
    lastInput = millis();
    autoMode = false;
  }

  if (any && showStatus) {
    // любая кнопка закрывает экран статуса
    showStatus = false;
    drawAnimated(-1);
  } else if (es) {
    showStatus = true;
    drawAnimated(+1);
  } else if (ev) {
    viewMode = (ViewMode)((viewMode + 1) % 3);
    drawAnimated(+1);
  } else if (en) {
    curIdx = (curIdx + 1) % NC;
    drawAnimated(+1);
  } else if (ep) {
    curIdx = (curIdx + NC - 1) % NC;
    drawAnimated(-1);
  } else if (eu && viewMode == VIEW_CONV) {
    convAmtIdx = min(convAmtIdx + 1, CONV_N - 1);
    draw();
  } else if (ed && viewMode == VIEW_CONV) {
    convAmtIdx = max(convAmtIdx - 1, 0);
    draw();
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

  // сеть принесла новые данные или сменила состояние — перерисовать
  if (dataDirty) {
    dataDirty = false;
    draw();
  }

  // мигание точки, пока в фоне идёт загрузка
  bool wantDot = (busy != BUSY_NONE) && ((millis() / 400) & 1);

  if (wantDot != dotShown) {
    dotShown = wantDot;
    draw();
  }

  if (showStatus && millis() - lastStatusDraw >= 1000) {
    lastStatusDraw = millis();
    draw();
  }

  delay(2);
}