// WiFi/mDNS, the /api/usage poll, and its SD-backed cache/archive siblings —
// split out of cyd_dashboard.ino's original "NETWORK" section.
#include "state.h"

// ── NETWORK ────────────────────────────────────────────────
// (Re)start the mDNS resolver whenever WiFi comes up — needed both after the
// first connect and after any reconnect (the board's own IP may have changed).
bool mdnsStarted = false;

void ensureMdns() {
  if (!mdnsStarted && MDNS.begin("cyd-dashboard")) {
    mdnsStarted = true;
  }
}

// 8 dots arranged in a circle, one lit at a time to give a rotating-spinner
// effect while connectWifi() blocks waiting for an AP.
static const int SPINNER_DOTS = 8;

static void drawWifiSpinner(int frameNum) {
  const int cx = 152, cy = 120, r = 24, dotR = 4;
  g->fillScreen(COL_BG);
  for (int i = 0; i < SPINNER_DOTS; i++) {
    float angle = i * 2 * PI / SPINNER_DOTS;
    int x = cx + (int)(r * cosf(angle));
    int y = cy + (int)(r * sinf(angle));
    g->fillCircle(x, y, dotR, i == frameNum % SPINNER_DOTS ? COL_ACCENT : COL_BORDER);
  }
  presentFrame();
}

void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);           // steadier link for an always-powered display
  WiFi.setAutoReconnect(true);    // rejoin automatically when the AP returns
  WiFi.persistent(true);
  WiFi.begin(cfgWifiSsid.c_str(), cfgWifiPassword.c_str());
  uint32_t start = millis();
  int frameNum = 0;
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    drawWifiSpinner(frameNum++);
    delay(100);
  }
  wifiOk = WiFi.status() == WL_CONNECTED;
  if (wifiOk) {
    ensureMdns();
    configTime(GMT_OFFSET_SEC, DST_OFFSET_SEC, "pool.ntp.org", "time.nist.gov");
  }
}

// Resolve cfgServerHost to an IP. Plain IPs are parsed directly; ".local"
// names are looked up over mDNS so the board tracks the Mac across IP changes.
static IPAddress serverIp;
static bool serverIpResolved = false;

bool resolveServer() {
  String host = cfgServerHost;
  if (host.endsWith(".local")) {
    String name = host.substring(0, host.length() - 6);
    IPAddress ip = MDNS.queryHost(name.c_str(), 3000);
    if (ip == IPAddress(0, 0, 0, 0)) return false;
    serverIp = ip;
  } else if (!serverIp.fromString(host)) {
    return false;
  }
  serverIpResolved = true;
  return true;
}

static const char* CACHE_PATH = "/last_usage.json";
static const char* WEATHER_CACHE_PATH = "/weather.json";

// Apply one weather object (live /api/usage or SD /weather.json) into STATE.
// Caller must hold stateMutex. Keeps prior values when a field is absent so a
// partial payload (old server, or last_env-style cache) never blanks the page.
static void applyWeatherDoc(JsonObjectConst w) {
  if (w.isNull()) return;
  if (!w["tempC"].isNull()) STATE.weatherTempC = w["tempC"].as<float>();
  if (!w["code"].isNull()) STATE.weatherCode = w["code"].as<int>();
  // high/low may be JSON null when the provider omitted them — only write
  // when the value is actually numeric.
  if (w["high"].is<float>() || w["high"].is<int>() || w["high"].is<double>()) {
    STATE.weatherHigh = (int)round(w["high"].as<double>());
  }
  if (w["low"].is<float>() || w["low"].is<int>() || w["low"].is<double>()) {
    STATE.weatherLow = (int)round(w["low"].as<double>());
  }
  if (w["condition"].is<const char*>()) {
    const char* c = w["condition"].as<const char*>();
    if (c) {
      strncpy(STATE.weatherCondition, c, sizeof(STATE.weatherCondition) - 1);
      STATE.weatherCondition[sizeof(STATE.weatherCondition) - 1] = '\0';
    }
  }
  if (w["place"].is<const char*>()) {
    const char* p = w["place"].as<const char*>();
    if (p) {
      strncpy(STATE.weatherPlace, p, sizeof(STATE.weatherPlace) - 1);
      STATE.weatherPlace[sizeof(STATE.weatherPlace) - 1] = '\0';
    }
  }
  if (w["hourly"].is<JsonArrayConst>()) {
    JsonArrayConst arr = w["hourly"].as<JsonArrayConst>();
    uint8_t n = 0;
    for (JsonObjectConst h : arr) {
      if (n >= WEATHER_HOURLY_N) break;
      STATE.weatherHourly[n].hour = (int8_t)(h["h"] | -1);
      STATE.weatherHourly[n].tempC = (int8_t)round((double)(h["tempC"] | 0.0));
      STATE.weatherHourly[n].code = (int16_t)(h["code"] | -1);
      n++;
    }
    STATE.weatherHourlyCount = n;
  }
  if (w["daily"].is<JsonArrayConst>()) {
    JsonArrayConst arr = w["daily"].as<JsonArrayConst>();
    uint8_t n = 0;
    for (JsonObjectConst d : arr) {
      if (n >= WEATHER_DAILY_N) break;
      STATE.weatherDaily[n].wday = (int8_t)(d["wd"] | -1);
      STATE.weatherDaily[n].high = (int8_t)round((double)(d["high"] | 0.0));
      STATE.weatherDaily[n].low = (int8_t)round((double)(d["low"] | 0.0));
      STATE.weatherDaily[n].code = (int16_t)(d["code"] | -1);
      n++;
    }
    STATE.weatherDailyCount = n;
  }
}

// Parses one /api/usage payload into STATE. Shared by a live fetch and a
// cached-on-SD read, so a stale card-backed copy renders identically to a
// fresh one. Uses an ArduinoJson filter so only the keys the board actually
// reads (not e.g. "clients"/"generated_at"/"last_activity_sec") get copied
// into the parsed JsonDocument's memory pool.
static JsonDocument usageFilter() {
  JsonDocument f;
  f["projects"][0]["name"] = true;
  f["projects"][0]["tokens"] = true;
  f["trend"] = true;
  f["last24h"]["tokens"] = true;
  f["limits"]["session"] = true;
  f["limits"]["week"] = true;
  f["limits"]["week_model"] = true;
  f["limits"]["credits"] = true;
  f["context"] = true;
  f["btc"] = true;
  f["weather"] = true;
  f["today"] = true;  // needed by appendArchiveRow's caller (fetchUsage)
  f["active_now"] = true;
  return f;
}

bool applyUsageJson(const String& payload) {
  JsonDocument doc;
  static JsonDocument filter = usageFilter();
  DeserializationError err = deserializeJson(doc, payload, DeserializationOption::Filter(filter));
  if (err) return false;

  // Parse above runs on a local doc (no shared state); take the lock only for
  // the copy into STATE below, which the render loop reads concurrently.
  lockState();
  JsonArray projects = doc["projects"].as<JsonArray>();
  STATE.projectCount = 0;
  for (JsonObject p : projects) {
    if (STATE.projectCount >= 5) break;
    snprintf(STATE.projectNames[STATE.projectCount], sizeof(STATE.projectNames[0]),
             "%s", p["name"] | "");
    STATE.projectTokens[STATE.projectCount] = p["tokens"] | (int64_t)0;
    STATE.projectCount++;
  }

  JsonArray trend = doc["trend"].as<JsonArray>();
  for (int i = 0; i < 7; i++) {
    STATE.trend[i] = i < (int)trend.size() ? trend[i].as<int64_t>() : 0;
  }
  STATE.last24hTokens = doc["last24h"]["tokens"] | (int64_t)0;

  if (!doc["limits"].isNull()) {
    STATE.sessionPercent = doc["limits"]["session"]["percent"] | -1;
    snprintf(STATE.sessionResets, sizeof(STATE.sessionResets), "%s",
             doc["limits"]["session"]["resets"] | "");
    STATE.sessionResetsInSec = doc["limits"]["session"]["resets_in_sec"] | -1L;
    STATE.weekPercent = doc["limits"]["week"]["percent"] | -1;
    snprintf(STATE.weekResets, sizeof(STATE.weekResets), "%s",
             doc["limits"]["week"]["resets"] | "");
    STATE.weekResetsInSec = doc["limits"]["week"]["resets_in_sec"] | -1L;
    STATE.weekModelPercent = doc["limits"]["week_model"]["percent"] | -1;
    snprintf(STATE.weekModelName, sizeof(STATE.weekModelName), "%s",
             doc["limits"]["week_model"]["name"] | "");
    snprintf(STATE.weekModelResets, sizeof(STATE.weekModelResets), "%s",
             doc["limits"]["week_model"]["resets"] | "");
    STATE.creditsUsed = doc["limits"]["credits"]["used"] | -1.0f;
    STATE.creditsLimit = doc["limits"]["credits"]["limit"] | -1.0f;
    STATE.creditsPercent = doc["limits"]["credits"]["percent"] | -1;
  } else {
    STATE.sessionPercent = -1;
    STATE.sessionResets[0] = '\0';
    STATE.sessionResetsInSec = -1L;
    STATE.weekPercent = -1;
    STATE.weekResets[0] = '\0';
    STATE.weekResetsInSec = -1L;
    STATE.weekModelPercent = -1;
    STATE.weekModelName[0] = '\0';
    STATE.weekModelResets[0] = '\0';
    STATE.creditsUsed = -1;
    STATE.creditsLimit = -1;
    STATE.creditsPercent = -1;
  }

  // Context window of the latest session — computed by the server from the
  // newest jsonl event; null until its first scan completes.
  if (!doc["context"].isNull()) {
    STATE.ctxTokens = doc["context"]["tokens"] | -1L;
    STATE.ctxPercent = doc["context"]["percent"] | -1;
  } else {
    STATE.ctxTokens = -1;
    STATE.ctxPercent = -1;
  }

  // BTC + weather are fetched by the Mac and delivered in this same payload (the
  // board can't do TLS — see the server's market_loop). Keep the last known
  // value if a field is absent so the tiles don't flicker to "--".
  if (!doc["btc"].isNull()) {
    double p = doc["btc"]["price"] | -1.0;
    if (p > 0) STATE.btcPrice = p;
    STATE.btcChangePct = doc["btc"]["changePct"] | STATE.btcChangePct;
  }
  if (!doc["weather"].isNull()) {
    applyWeatherDoc(doc["weather"].as<JsonObjectConst>());
  }

  STATE.lastFetchOkMs = millis();
  unlockState();
  return true;
}

// Reads the last-good cached response written by fetchUsage() below, so the
// dashboard can keep showing real (if stale) data instead of going blank
// when the Mac is unreachable. Small bounded file — buffered read is fine.
bool loadCachedUsage() {
  lockSD();
  File f = SD.open(CACHE_PATH, FILE_READ);
  String payload = f ? f.readString() : String();
  if (f) f.close();
  unlockSD();
  if (!payload.length()) return false;
  return applyUsageJson(payload);  // applyUsageJson takes stateMutex — kept outside sdMutex
}

static const char* ENV_CACHE_PATH = "/last_env.json";
static const char* ARCHIVE_PATH = "/archive.csv";

// Persist the latest BTC price + Bangkok weather so those tiles show
// last-known values immediately on a cold boot (and while offline) instead of
// "--" until the first live fetch lands. Written once per usage poll (~20s) to
// bound card wear, not on every 5s BTC refresh.
static void saveEnvCache() {
  if (!STATE.sdOk) return;
  lockState();
  double btc = STATE.btcPrice;
  float changePct = STATE.btcChangePct;
  float tempC = STATE.weatherTempC;
  int code = STATE.weatherCode;
  unlockState();
  SD.remove(ENV_CACHE_PATH);  // FILE_WRITE appends here; remove for a clean overwrite
  File f = SD.open(ENV_CACHE_PATH, FILE_WRITE);
  if (f) {
    f.printf("{\"btc\":%.2f,\"changePct\":%.2f,\"tempC\":%.1f,\"code\":%d}", btc, changePct, tempC, code);
    f.close();
  }
}

void loadEnvCache() {
  File f = SD.open(ENV_CACHE_PATH, FILE_READ);
  if (!f) return;
  String payload = f.readString();
  f.close();
  JsonDocument doc;
  if (deserializeJson(doc, payload)) return;
  lockState();
  STATE.btcPrice = doc["btc"] | STATE.btcPrice;
  STATE.btcChangePct = doc["changePct"] | STATE.btcChangePct;
  STATE.weatherTempC = doc["tempC"] | STATE.weatherTempC;
  STATE.weatherCode = doc["code"] | STATE.weatherCode;
  unlockState();
}

// Full Weather-page snapshot (current + 6h + 5d). SD is this board's local
// DB — the project never mounts SPIFFS/LittleFS (huge_app partition, all
// persistence is the card; see CLAUDE.md). Written once per successful
// usage poll so the board's Poll Interval setting paces the refresh.
static void saveWeatherCache() {
  if (!STATE.sdOk) return;
  lockState();
  if (STATE.weatherTempC < -900 && STATE.weatherHourlyCount == 0) {
    unlockState();
    return;  // nothing useful to persist yet
  }
  JsonDocument doc;
  doc["tempC"] = STATE.weatherTempC;
  doc["code"] = STATE.weatherCode;
  if (STATE.weatherHigh > -900) doc["high"] = STATE.weatherHigh;
  if (STATE.weatherLow > -900) doc["low"] = STATE.weatherLow;
  doc["condition"] = STATE.weatherCondition;
  doc["place"] = STATE.weatherPlace;
  JsonArray hourly = doc["hourly"].to<JsonArray>();
  for (uint8_t i = 0; i < STATE.weatherHourlyCount; i++) {
    JsonObject h = hourly.add<JsonObject>();
    h["h"] = STATE.weatherHourly[i].hour;
    h["tempC"] = STATE.weatherHourly[i].tempC;
    h["code"] = STATE.weatherHourly[i].code;
  }
  JsonArray daily = doc["daily"].to<JsonArray>();
  for (uint8_t i = 0; i < STATE.weatherDailyCount; i++) {
    JsonObject d = daily.add<JsonObject>();
    d["wd"] = STATE.weatherDaily[i].wday;
    d["high"] = STATE.weatherDaily[i].high;
    d["low"] = STATE.weatherDaily[i].low;
    d["code"] = STATE.weatherDaily[i].code;
  }
  unlockState();

  // Serialize outside the lock (string build can allocate); card write is
  // under the caller's sdMutex (fetchUsage).
  String payload;
  serializeJson(doc, payload);
  SD.remove(WEATHER_CACHE_PATH);
  File f = SD.open(WEATHER_CACHE_PATH, FILE_WRITE);
  if (f) {
    f.print(payload);
    f.close();
  }
}

void loadWeatherCache() {
  File f = SD.open(WEATHER_CACHE_PATH, FILE_READ);
  if (!f) return;
  String payload = f.readString();
  f.close();
  JsonDocument doc;
  if (deserializeJson(doc, payload)) return;
  lockState();
  applyWeatherDoc(doc.as<JsonObjectConst>());
  unlockState();
}

// Fine-grained, append-only history — one row per successful poll (~20s),
// unbounded (that's the point of the roomy card; ~1GB/year). Meant for
// off-device analysis, not shown on screen — distinct from /daily_log.csv,
// which keeps only one end-of-day row for the on-screen 30-day trend. Takes
// the already-parsed doc (fetchUsage parses the payload once via
// applyUsageJson; this used to re-deserialize the same payload a second time).
static void appendArchiveRow(const JsonDocument& doc) {
  if (!STATE.sdOk) return;

  struct tm timeinfo;
  char tsBuf[24];
  if (!getLocalTime(&timeinfo, 0)) return;  // no wall-clock yet — skip, don't write a bogus ts
  snprintf(tsBuf, sizeof(tsBuf), "%04d-%02d-%02d %02d:%02d:%02d",
           timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
           timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);

  int64_t todayTokens = doc["today"]["tokens"] | (int64_t)0;
  float todayCost = doc["today"]["cost"] | 0.0f;
  int active = (doc["active_now"] | false) ? 1 : 0;

  lockState();
  int sessionPct = STATE.sessionPercent;
  int weekPct = STATE.weekPercent;
  double btc = STATE.btcPrice;
  float tempC = STATE.weatherTempC;
  unlockState();

  bool isNew = !SD.exists(ARCHIVE_PATH);
  File f = SD.open(ARCHIVE_PATH, FILE_APPEND);
  if (!f) return;
  if (isNew) f.println("ts,today_tokens,today_cost,session_pct,week_pct,active,btc,tempC");
  f.printf("%s,%lld,%.4f,%d,%d,%d,%.2f,%.1f\n",
           tsBuf, (long long)todayTokens, todayCost,
           sessionPct, weekPct, active, btc, tempC);
  f.close();
}

bool fetchUsage() {
  uint32_t startUs = micros();  // diagnostic timing only, see comment at the end of this function
  if (WiFi.status() != WL_CONNECTED) {
    wifiOk = false;
    return false;
  }
  wifiOk = true;

  if (!serverIpResolved && !resolveServer()) {
    return false;  // Mac not announcing itself yet (likely still asleep)
  }

  HTTPClient http;
  String url = String("http://") + serverIp.toString() + ":" + String(cfgServerPort) + "/api/usage";
  http.setConnectTimeout(3000);  // cap the TCP connect, not just the read
  http.setTimeout(5000);
  http.begin(url);
  int code = http.GET();
  if (code != 200) {
    http.end();
    serverIpResolved = false;  // force a fresh mDNS lookup next cycle
    return false;
  }

  String payload = http.getString();
  http.end();

  // Parsed once here (filtered) and reused below for the archive row, rather
  // than applyUsageJson() and appendArchiveRow() each deserializing their own
  // copy of the same payload.
  JsonDocument doc;
  static JsonDocument filter = usageFilter();
  DeserializationError err = deserializeJson(doc, payload, DeserializationOption::Filter(filter));
  if (err) return false;
  if (!applyUsageJson(payload)) return false;

  if (STATE.sdOk) {
    // FILE_WRITE appends on this SD library rather than truncating, so
    // remove first to guarantee a clean overwrite each time. One sdMutex hold
    // covers the cache write + archive + env cache (appendArchiveRow/saveEnvCache
    // do NOT self-lock the card — they run only from inside this block).
    lockSD();
    SD.remove(CACHE_PATH);
    File f = SD.open(CACHE_PATH, FILE_WRITE);
    if (f) {
      f.print(payload);
      f.close();
    }
    appendArchiveRow(doc);      // fine-grained per-poll history for later analysis
    saveEnvCache();             // refresh the BTC/weather cold-boot cache
    saveWeatherCache();         // full Weather-page snapshot (paced by poll interval)
    unlockSD();
  }

  // Diagnostic timing only (one line per successful poll, ~20s cadence) --
  // kept permanently, low-volume, so a future regression in the fetch/parse/
  // SD-write path shows up the same way the JSON-filter and single-parse
  // optimizations here were verified.
  Serial.printf("[timing] fetchUsage() took %luus\n", (unsigned long)(micros() - startUs));
  return true;
}

// BTC price and Bangkok weather are NOT fetched on the board. This unit has no
// PSRAM, and the 154KB framebuffer leaves too little contiguous heap for an
// mbedTLS handshake (verified: TCP 443 connects but the TLS buffer allocation
// fails). Instead the Mac fetches both and includes them in /api/usage, which
// the board reads over plain HTTP; applyUsageJson() copies them into STATE.
