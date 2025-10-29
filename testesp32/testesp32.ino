/********** ESP32 Garden -> Google Sheets + JSONBin Remote Pump (0=auto,1=force_on) **********/
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>

/* ===== Struct ===== */
struct FSum {
  bool rain;
  float maxPOP;
  float maxR3h;
};

/* ===== WiFi ===== */
const char* ssid = "AKE";
const char* password = "0908637046";

/* ===== Location ===== */
const float LAT = 14.0009, LON = 99.5398;

/* ===== OpenWeather ===== */
const char* API_KEY = "35a9a4dc1f6261526fdbc03d4b7eb666";
String buildForecastURL() {
  String url = "http://api.openweathermap.org/data/2.5/forecast";
  url += "?lat=" + String(LAT, 6);
  url += "&lon=" + String(LON, 6);
  url += "&appid=" + String(API_KEY);
  url += "&units=metric&lang=th";
  return url;
}

/* ===== Pins & Threshold ===== */
const int PIN_PUMP = 23;
const int PIN_SOIL = 34;
const int PIN_WATER = 35;

int TH_DRY = 3000;  // > TH_DRY => ดินแห้ง
int TH_WET = 2500;  // < TH_WET => ดินชื้น
const int HOURS_LOOKAHEAD = 9;
float POP_TH = 0.5;
float RAIN_TH = 1.0;

/* ===== Pump guards ===== */
unsigned long MIN_ON = 60UL * 1000;
unsigned long MIN_OFF = 60UL * 1000;
unsigned long lastSw = 0;
bool pumpOn = false;

/* ===== Forecast cache ===== */
unsigned long lastForecast = 0;
const unsigned long forecastInterval = 15UL * 60UL * 1000;
FSum cachedF = { false, 0, 0 };

/* ===== Google Sheets Web App URL ===== */
const char* GSCRIPT_URL = "https://script.google.com/macros/s/AKfycbw5EzzEbt38WhpWfS1qa02Xr83ynFnBKyBX5PAp-9MUbHdbcAwA36KcGceuQvi2nFJqDw/exec";

/* ===== JSONBin (Remote pump control) ===== */
const char* JSONBIN_URL = "https://api.jsonbin.io/v3/b/68fe94c7ae596e708f2ebc87/latest";
unsigned long lastRemotePoll = 0;
const unsigned long remotePollInterval = 5000;  // 5s

/* ===== Simulation/Override modes ===== */
bool simRain = false, simSoil = false, simPump = false, simWater = false;
bool simRainVal = false, simSoilDry = false, simPumpOn = false, simWaterOK = true;

/* ===== Global states ===== */
int lastSoilADC = 0, lastWaterADC = 0;
bool soilDry = false;

unsigned long lastPushSheets = 0;
const unsigned long pushSheetsInterval = 60UL * 1000;

/* ===== Utils ===== */
bool ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  unsigned long t0 = millis();
  Serial.print("WiFi");
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    Serial.print(".");
    delay(400);
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("IP: %s RSSI:%d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    return true;
  }
  Serial.println("WiFi fail");
  return false;
}
int readSoilAvg(int n = 8) {
  long s = 0;
  for (int i = 0; i < n; i++) {
    s += analogRead(PIN_SOIL);
    delay(5);
  }
  return s / n;
}
bool waterOKReal() {
  return analogRead(PIN_WATER) > 500;
}

bool canSwitch(unsigned long req) {
  return millis() - lastSw >= req;
}
void setPump(bool on) {
  if (simPump) {
    pumpOn = simPumpOn;
    digitalWrite(PIN_PUMP, pumpOn ? HIGH : LOW);
    return;
  }
  if (on == pumpOn) return;
  if (on && !canSwitch(MIN_OFF)) {
    Serial.println("รอช่วง OFF ให้ครบก่อน");
    return;
  }
  if (!on && !canSwitch(MIN_ON)) {
    Serial.println("รอช่วง ON ให้ครบก่อน");
    return;
  }
  pumpOn = on;
  digitalWrite(PIN_PUMP, on ? HIGH : LOW);
  lastSw = millis();
  Serial.println(on ? "Pump ON" : "Pump OFF");
}

/* ===== Forecast ===== */
FSum fetchForecast() {
  FSum r{ false, 0, 0 };
  if (!ensureWiFi()) return r;
  WiFiClient client;
  HTTPClient http;
  http.setReuse(false);
  http.setTimeout(15000);
  http.begin(client, buildForecastURL());
  int code = http.GET();
  if (code == HTTP_CODE_OK) {
    StaticJsonDocument<16384> doc;
    if (!deserializeJson(doc, http.getString())) {
      JsonArray list = doc["list"];
      int steps = HOURS_LOOKAHEAD / 3;
      if (steps < 1) steps = 1;
      if (steps > (int)list.size()) steps = list.size();
      for (int i = 0; i < steps; i++) {
        JsonObject it = list[i];
        float pop = it["pop"] | 0.0;
        float r3h = it.containsKey("rain") ? (it["rain"]["3h"] | 0.0) : 0.0;
        const char* mainW = it["weather"][0]["main"] | "";

        // เก็บค่ามากสุดไว้ใช้ทีหลัง
        if (pop > r.maxPOP) r.maxPOP = pop;
        if (r3h > r.maxR3h) r.maxR3h = r3h;

        if (pop >= POP_TH || r3h >= RAIN_TH || strcmp(mainW, "Rain") == 0)
          r.rain = true;
      }
    } else Serial.println("JSON parse error");
  } else Serial.printf("HTTP error: %d\n", code);
  http.end();
  return r;
}

/* ===== RAW HTTPS helpers (POST->GAS with redirect follow) ===== */
static bool splitHostPath(const String& url, String& host, String& path) {
  String u = url;
  int p = u.indexOf("://");
  if (p >= 0) u = u.substring(p + 3);
  int slash = u.indexOf('/');
  if (slash < 0) {
    host = u;
    path = "/";
  } else {
    host = u.substring(0, slash);
    path = u.substring(slash);
  }
  if (!path.startsWith("/")) path = "/" + path;
  return host.length() > 0;
}
static String readLine(WiFiClientSecure& c, uint32_t to = 12000) {
  uint32_t t0 = millis();
  String s;
  while (millis() - t0 < to) {
    while (c.available()) {
      char ch = (char)c.read();
      if (ch == '\r') continue;
      if (ch == '\n') return s;
      s += ch;
    }
    delay(1);
  }
  return s;
}
static bool tlsConnect(WiFiClientSecure& cli, const String& host, uint16_t port = 443) {
  cli.setInsecure();
  cli.setTimeout(15000);
  for (int i = 0; i < 2; i++) {
    if (cli.connect(host.c_str(), port)) return true;
    delay(250);
  }
  return false;
}
bool postJSON_GAS_FOLLOW_RAW(const String& url, const String& body, int maxRedirects = 3) {
  if (WiFi.status() != WL_CONNECTED && !ensureWiFi()) return false;
  String cur = url, postBody = body;

  for (int hop = 0; hop <= maxRedirects; hop++) {
    String host, path;
    if (!splitHostPath(cur, host, path)) {
      Serial.println("URL ผิดรูปแบบ");
      return false;
    }

    WiFiClientSecure cli;
    if (!tlsConnect(cli, host)) {
      Serial.println("เชื่อม TLS ไม่ได้ (POST)");
      return false;
    }

    String req;
    req = "POST " + path + " HTTP/1.1\r\n";
    req += "Host: " + host + "\r\n";
    req += "User-Agent: ESP32\r\n";
    req += "Accept: application/json, */*\r\n";
    req += "Accept-Encoding: identity\r\n";
    req += "Content-Type: application/json; charset=utf-8\r\n";
    req += "Connection: close\r\n";
    req += "Content-Length: " + String(postBody.length()) + "\r\n\r\n";
    cli.print(req);
    if (postBody.length()) cli.write((const uint8_t*)postBody.c_str(), postBody.length());

    String status = readLine(cli);
    status.trim();
    Serial.print("Status: ");
    Serial.println(status);
    String location = "";
    while (true) {
      String h = readLine(cli);
      if (h.length() == 0) break;
      int colon = h.indexOf(':');
      if (colon > 0) {
        String k = h.substring(0, colon);
        k.toLowerCase();
        String v = h.substring(colon + 1);
        v.trim();
        if (k == "location") location = v;
      }
    }

    if (status.startsWith("HTTP/1.1 200") || status.startsWith("HTTP/1.0 200")) {
      cli.stop();
      return true;
    }

    // 302/303 -> GET ใหม่ด้วย client ใหม่
    if ((status.startsWith("HTTP/1.1 302") || status.startsWith("HTTP/1.1 303")) && location.length()) {
      if (location.startsWith("/")) location = "https://" + host + location;
      Serial.print("Redirect (GET) to: ");
      Serial.println(location);
      cli.stop();

      if (!splitHostPath(location, host, path)) return false;

      WiFiClientSecure cli2;
      if (!tlsConnect(cli2, host)) {
        Serial.println("TLS connect fail (redirect GET)");
        return false;
      }

      String getReq;
      getReq = "GET " + path + " HTTP/1.1\r\n";
      getReq += "Host: " + host + "\r\nUser-Agent: ESP32\r\nAccept: */*\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n";
      cli2.print(getReq);

      String st2 = readLine(cli2);
      st2.trim();
      Serial.print("Status: ");
      Serial.println(st2);
      if (st2.startsWith("HTTP/1.1 200") || st2.startsWith("HTTP/1.0 200")) {
        cli2.stop();
        return true;
      }
      Serial.println("----- Response (debug) -----");
      while (cli2.connected() || cli2.available()) Serial.print(cli2.readString());
      Serial.println("\n----------------------------");
      cli2.stop();
      return false;
    }

    // 307/308 -> POST ต่อที่ Location (วน hop ต่อไป)
    if ((status.startsWith("HTTP/1.1 307") || status.startsWith("HTTP/1.1 308")) && location.length()) {
      if (location.startsWith("/")) location = "https://" + host + location;
      Serial.print("Redirect (POST) to: ");
      Serial.println(location);
      cli.stop();
      cur = location;
      continue;
    }

    Serial.println("----- Response (debug) -----");
    while (cli.connected() || cli.available()) Serial.print(cli.readString());
    Serial.println("\n----------------------------");
    cli.stop();
    return false;
  }
  Serial.println("ตามรีไดเรกต์เกินที่กำหนด");
  return false;
}

/* ===== ดึงค่า pump จาก JSONBin ===== */
int fetchRemotePumpCmd() {
  if (!ensureWiFi()) return -1;
  WiFiClientSecure cli;
  HTTPClient http;
  cli.setInsecure();
  http.setTimeout(8000);
  if (!http.begin(cli, JSONBIN_URL)) {
    Serial.println("JSONBin begin fail");
    return -1;
  }
  int code = http.GET();
  if (code == HTTP_CODE_OK) {
    DynamicJsonDocument doc(2048);
    DeserializationError err = deserializeJson(doc, http.getString());
    if (err) {
      Serial.printf("JSONBin parse err: %s\n", err.c_str());
      http.end();
      return -1;
    }
    JsonVariant v = doc["record"]["record"]["record"]["pump"];
    if (!v.isNull()) {
      int p = v.as<int>();
      http.end();
      if (p == 0 || p == 1) return p;  // 0=auto, 1=force on
    }
  } else {
    Serial.printf("JSONBin HTTP err: %d\n", code);
  }
  http.end();
  return -1;
}

/* ===== ส่งข้อมูลขึ้น Google Sheets (มีแหล่งที่มาครบ) ===== */
bool sendToGoogleSheets(const FSum& f, bool soilDry, bool pumpOn,
                        const char* pumpCmd, bool haveWaterEff,
                        int soilPct, int waterBin) {
  StaticJsonDocument<512> doc;

  // ข้อมูลหลัก
  doc["device_id"] = "esp32-kan-01";
  doc["soil_pct"] = soilPct;
  doc["water_bin"] = waterBin;
  doc["soil_status"] = soilDry ? "ดินแห้ง" : "ดินชื้น";
  doc["water_status"] = haveWaterEff ? "มีน้ำ" : "ไม่มีน้ำ";
  doc["pump_status"] = pumpOn ? "ทำงาน" : "หยุด";
  doc["pop_max"] = (int)roundf(f.maxPOP * 100.0f);  // จะได้ค่าระหว่าง 0-100 จริง
  doc["rain3h_max"] = f.maxR3h;                     // ปริมาณฝน (mm/3h)
  doc["rain"] = f.rain ? "มีฝน" : "ไม่มีฝน";          // แหล่งที่มา (คืนครบ)
  doc["rain_src"] = simRain ? "sim" : "forecast";
  doc["soil_src"] = simSoil ? "sim" : "sensor";
  doc["water_src"] = simWater ? "sim" : "sensor";
  doc["pump_src"] = simPump ? "remote" : "auto";  // remote = JSONBin/Serial
  doc["pump_cmd"] = pumpCmd;                      // force_on/force_off/auto_on/auto_off

  // ตำแหน่ง & IP
  doc["lat"] = LAT;
  doc["lon"] = LON;
  doc["ip"] = WiFi.localIP().toString();

  String body;
  serializeJson(doc, body);
  Serial.println("📤 JSON ส่งขึ้น: " + body);
  return postJSON_GAS_FOLLOW_RAW(GSCRIPT_URL, body);
}

/* ===== Serial (optional) ===== */
void processCommand(String cmd) {
  cmd.trim();
  cmd.toLowerCase();
  if (cmd == "pump on") {
    simPump = true;
    simPumpOn = true;
    digitalWrite(PIN_PUMP, HIGH);
    pumpOn = true;
    Serial.println("Manual pump ON");
  }
  if (cmd == "pump off") {
    simPump = true;
    simPumpOn = false;
    digitalWrite(PIN_PUMP, LOW);
    pumpOn = false;
    Serial.println("Manual pump OFF");
  }
  if (cmd == "pump auto") {
    simPump = false;
    Serial.println("Back to AUTO");
  }
}

/* ===== Setup / Loop ===== */
void setup() {
  Serial.begin(115200);
  pinMode(PIN_PUMP, OUTPUT);
  digitalWrite(PIN_PUMP, LOW);
  analogSetAttenuation(ADC_11db);
  ensureWiFi();
  configTime(7 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  Serial.println("🌱 Garden system started");
}

void loop() {
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    processCommand(cmd);
  }

  // ======= Remote pump control via JSONBin (0=auto,1=force_on) =======
  if (millis() - lastRemotePoll > remotePollInterval) {
    int remote = fetchRemotePumpCmd();  // -1 = no change
    if (remote == 1) {
      // Force ON
      simPump = true;
      simPumpOn = true;
      digitalWrite(PIN_PUMP, HIGH);
      pumpOn = true;
      Serial.println("🔌 Remote pump: FORCE ON");
    } else if (remote == 0) {
      // Back to AUTO
      simPump = false;
      Serial.println("🌿 Remote pump: AUTO MODE");
    }
    lastRemotePoll = millis();
  }

  // ======= Sensors / logic (auto mode) =======
  lastSoilADC = readSoilAvg();
  lastWaterADC = analogRead(PIN_WATER);

  if (!simSoil) {
    if (!soilDry) {
      if (lastSoilADC > TH_DRY) soilDry = true;
    } else {
      if (lastSoilADC < TH_WET) soilDry = false;
    }
  } else soilDry = simSoilDry;

  bool haveWaterReal = waterOKReal();
  bool haveWater = simWater ? simWaterOK : haveWaterReal;
  if (!haveWater) { setPump(false); }

  if (millis() - lastForecast > forecastInterval) {
    cachedF = fetchForecast();
    lastForecast = millis();
  }
  bool rainNow = simRain ? simRainVal : cachedF.rain;

  bool wantOn = (!simPump) && soilDry && !rainNow && haveWater;
  setPump(wantOn);

  int soilPct = (int)roundf((4095.0f - lastSoilADC) * 100.0f / 4095.0f);
  soilPct = constrain(soilPct, 0, 100);
  int waterBin = haveWater ? 1 : 0;

  if (millis() - lastPushSheets > pushSheetsInterval) {
    const char* pumpCmd =
      simPump ? (simPumpOn ? "force_on" : "force_off")
              : (wantOn ? "auto_on" : "auto_off");
    FSum f2 = cachedF;
    f2.rain = rainNow;
    bool ok = sendToGoogleSheets(f2, soilDry, pumpOn, pumpCmd, haveWater, soilPct, waterBin);
    Serial.println(ok ? "✅ ส่งข้อมูลสำเร็จ" : "❌ ส่งข้อมูลล้มเหลว");
    lastPushSheets = millis();
  }

  Serial.printf("soilADC=%d -> soil_pct=%d%% | waterADC=%d -> water_bin=%d | rain=%s | pump=%s | mode=%s\n",
                lastSoilADC, soilPct, lastWaterADC, waterBin, rainNow ? "RAIN" : "CLEAR", pumpOn ? "ON" : "OFF", simPump ? "FORCE" : "AUTO");

  delay(pumpOn ? 2000 : 3000);
}
