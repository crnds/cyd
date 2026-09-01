// WiFi/mDNS, the /api/usage poll, and its SD-backed cache/archive siblings —
// split out of cyd_dashboard.ino's original "NETWORK" section.
#include "state.h"
#include <sys/time.h>  // settimeofday() — NTP fallback in applyUsageJson

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
  const int cx = 160, cy = 120, r = 24, dotR = 4;
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
//
// mDNS must never become the ONLY road back online. Multicast is the first
// thing a busy or lossy WLAN drops, and MDNS.queryHost() blocks for up to 3s —
// at the 5s Poll Interval setting that is most of a cycle, every cycle. So:
//   * once an address has resolved even once, a later failed lookup keeps
//     using it rather than failing the poll (the Mac's IP only really changes
//     across a sleep/wake, so stale-but-plausible beats nothing at all);
//   * lookups are rate-limited to RESOLVE_RETRY_MS regardless of poll rate;
//   * the address is cached in NVS, so a reboot during a multicast outage can
//     still reach the Mac without a working lookup.
static IPAddress serverIp;
static bool serverIpValid = false;    // serverIp holds an address worth trying
static bool serverIpSuspect = false;  // ...but repeated failures say re-check it
static uint32_t lastResolveAttemptMs = 0;
static bool serverIpLoadedFromFlash = false;
static const uint32_t RESOLVE_RETRY_MS = 30000;

bool resolveServer() {
  String host = cfgServerHost;
  if (!host.endsWith(".local")) {
    if (!serverIp.fromString(host)) return false;
    serverIpValid = true;
    serverIpSuspect = false;
    return true;
  }

  // Seed from the NVS cache once per boot, so the very first poll after a
  // restart doesn't have to wait for a successful multicast lookup.
  if (!serverIpLoadedFromFlash) {
    serverIpLoadedFromFlash = true;
    uint32_t cached = loadServerIpFromFlash();
    if (cached != 0) {
      serverIp = IPAddress(cached);
      serverIpValid = true;
      serverIpSuspect = true;  // plausible, but confirm it with a real lookup
    }
  }

  uint32_t now = millis();
  if (lastResolveAttemptMs != 0 && now - lastResolveAttemptMs < RESOLVE_RETRY_MS) {
    return serverIpValid;  // too soon to pay for mDNS again; use what we have
  }
  lastResolveAttemptMs = now;

  String name = host.substring(0, host.length() - 6);
  IPAddress ip = MDNS.queryHost(name.c_str(), 3000);
  if (ip == IPAddress(0, 0, 0, 0)) {
    logDiag("mdns_resolve_fail");
    return serverIpValid;  // fall back to the last known good address
  }
  if (!serverIpValid || ip != serverIp) {
    serverIp = ip;
    saveIntConfigToFlash(SERVER_IP_KEY, (int32_t)(uint32_t)ip);
  }
  serverIpValid = true;
  serverIpSuspect = false;
  return true;
}

// Consecutive failed GETs against the currently cached serverIp. A single
// ordinary blip (one dropped packet, one slow response) shouldn't make us
// distrust an IP that's still almost certainly correct -- the Mac's IP only
// actually changes across a sleep/wake -- so this must reach
// IP_INVALIDATE_AFTER_FAILS before we mark the address suspect and let
// resolveServer() re-check it (which it then does on its own rate limit,
// still falling back to this address while the lookup keeps failing).
static int ipFailStreak = 0;
static const int IP_INVALIDATE_AFTER_FAILS = 2;

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
// reads (not e.g. "clients"/"generated_at") get copied into the pool.
static JsonDocument usageFilter() {
  JsonDocument f;
  f["projects"][0]["name"] = true;
  f["projects"][0]["tokens"] = true;
  f["trend"] = true;
  f["limits"]["session"] = true;
  f["limits"]["week"] = true;
  f["limits"]["week_model"] = true;
  f["limits"]["credits"] = true;
  f["context"] = true;
  f["btc"] = true;
  f["weather"] = true;
  f["aqi"] = true;
  f["note"] = true;   // NOTE_PAGE text + text size, authored in note.html
  f["today"] = true;  // needed by appendArchiveRow (fetchUsage)
  f["active_now"] = true;
  f["epoch"] = true;  // NTP fallback — see applyUsageDoc's time-sync block
  // Mac power / battery-save. Only battery_save is acted on.
  f["power"]["battery_save"] = true;
  return f;
}

// Does the actual STATE population from an already-parsed/filtered doc.
// Split out of applyUsageJson() so fetchUsage() -- which needs the parsed
// doc anyway for appendArchiveRow() -- can reuse that one parse instead of
// deserializing the same payload a second time via applyUsageJson(String).
static bool applyUsageDoc(const JsonDocument& doc, bool fromNetwork) {
  // NTP fallback: this board has no TLS and only ever reaches the public NTP
  // pool over plain UDP/123 at boot (see connectWifi()) — on networks that
  // block outbound UDP to the internet (hotel/guest WiFi, some routers) that
  // sync never lands and getLocalTime() fails forever, even though the plain
  // HTTP poll to the Mac (this very payload) works fine over the LAN. Whenever
  // we still don't have a wall clock, seed one from the server's epoch instead;
  // configTime()'s TZ (GMT_OFFSET_SEC/DST_OFFSET_SEC) was already set at boot
  // and keeps applying regardless of how time-of-day got set. Skipped once
  // getLocalTime() succeeds so a working NTP sync is never fought/overridden.
  struct tm haveTimeCheck;
  if (!getLocalTime(&haveTimeCheck, 0)) {
    long epoch = doc["epoch"] | 0L;
    if (epoch > 0) {
      struct timeval tv = { (time_t)epoch, 0 };
      settimeofday(&tv, nullptr);
    }
  }

  // Parse above runs on a local doc (no shared state); take the lock only for
  // the copy into STATE below, which the render loop reads concurrently.
  lockState();
  JsonArrayConst projects = doc["projects"].as<JsonArrayConst>();
  STATE.projectCount = 0;
  for (JsonObjectConst p : projects) {
    if (STATE.projectCount >= 5) break;
    snprintf(STATE.projectNames[STATE.projectCount], sizeof(STATE.projectNames[0]),
             "%s", p["name"] | "");
    STATE.projectTokens[STATE.projectCount] = p["tokens"] | (int64_t)0;
    STATE.projectCount++;
  }

  JsonArrayConst trend = doc["trend"].as<JsonArrayConst>();
  for (int i = 0; i < 7; i++) {
    STATE.trend[i] = i < (int)trend.size() ? trend[i].as<int64_t>() : 0;
  }

  // Keep the last-known values on a null/absent limits field, same as
  // btc/weather below — a transient miss shouldn't wipe a good snapshot to
  // "--", and under Battery Save's floored 120s poll interval a wipe would
  // stay visible for minutes instead of self-healing within one cycle.
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
  }
  if (!doc["weather"].isNull()) {
    applyWeatherDoc(doc["weather"].as<JsonObjectConst>());
  }
  if (!doc["aqi"].isNull()) {
    int a = doc["aqi"]["aqi"] | -1;
    if (a >= 0) STATE.aqi = a;
  }
  // The server always sends this object, so an empty "text" is a real
  // "the note was cleared" and must overwrite — unlike the guards above,
  // which keep the last-known value when a key is missing entirely.
  if (!doc["note"].isNull()) {
    snprintf(STATE.note, sizeof(STATE.note), "%s", doc["note"]["text"] | "");
    STATE.noteSize = constrain((int)(doc["note"]["size"] | 1), 1, 3);
  }

  STATE.lastFetchOkMs = millis();
  unlockState();

  // Mac battery-save flag: only from a live poll (fromNetwork). SD cache may
  // carry a stale power.battery_save from hours ago — applying that on boot
  // would floor the poll while the Mac is already back on AC. When AUTO mode
  // is selected, flip serverBatterySave + recompute POLL_INTERVAL_MS.
  if (fromNetwork && !doc["power"].isNull()) {
    bool want = doc["power"]["battery_save"] | false;
    if (want != serverBatterySave) {
      serverBatterySave = want;
      applyEffectivePoll();
    }
  }
  return true;
}

bool applyUsageJson(const String& payload, bool fromNetwork) {
  JsonDocument doc;
  static JsonDocument filter = usageFilter();
  DeserializationError err = deserializeJson(doc, payload, DeserializationOption::Filter(filter));
  if (err) return false;
  return applyUsageDoc(doc, fromNetwork);
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
  // fromNetwork=false: do not apply power.battery_save from a stale cache.
  return applyUsageJson(payload, false);
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
  float tempC = STATE.weatherTempC;
  int code = STATE.weatherCode;
  int aqi = STATE.aqi;
  unlockState();
  SD.remove(ENV_CACHE_PATH);  // FILE_WRITE appends here; remove for a clean overwrite
  File f = SD.open(ENV_CACHE_PATH, FILE_WRITE);
  if (f) {
    f.printf("{\"btc\":%.2f,\"tempC\":%.1f,\"code\":%d,\"aqi\":%d}", btc, tempC, code, aqi);
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
  STATE.weatherTempC = doc["tempC"] | STATE.weatherTempC;
  STATE.weatherCode = doc["code"] | STATE.weatherCode;
  STATE.aqi = doc["aqi"] | STATE.aqi;
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
// off-device analysis, not shown on screen. Takes the already-parsed doc
// (fetchUsage parses the payload once via applyUsageJson; this used to
// re-deserialize the same payload a second time).
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
  wifiOk = WiFi.status() == WL_CONNECTED;
  if (!wifiOk) {
    return false;
  }

  if ((!serverIpValid || serverIpSuspect) && !resolveServer()) {
    return false;  // never resolved at all — Mac likely still asleep
  }

  // One immediate retry before calling the cycle a failure. On a lossy link a
  // single dropped SYN is routine, and with the offline grace measured in wall
  // time (cyd_dashboard.ino's OFFLINE_AFTER_MS) an isolated miss should cost
  // nothing at all. Two back-to-back misses is real evidence, one is not.
  String payload;
  int code = 0;
  for (int attempt = 0; attempt < 2; attempt++) {
    if (attempt > 0) delay(300);
    HTTPClient http;
    String url = String("http://") + serverIp.toString() + ":" + String(cfgServerPort) + "/api/usage";
    http.setConnectTimeout(3000);  // cap the TCP connect, not just the read
    http.setTimeout(5000);
    http.begin(url);
    code = http.GET();
    if (code == 200) {
      payload = http.getString();
      http.end();
      break;
    }
    http.end();
  }

  if (code != 200) {
    ipFailStreak++;
    if (ipFailStreak == 1) {
      logDiag((String("fetch_fail http_code=") + code).c_str());
    } else if (ipFailStreak >= IP_INVALIDATE_AFTER_FAILS) {
      // Re-check the address, but keep using it meanwhile — resolveServer()
      // falls back to it for as long as the lookup keeps failing.
      serverIpSuspect = true;
    }
    return false;
  }
  // A 200 is direct proof the address is right, which beats any lookup — so
  // stop re-checking it. Without this, a board whose mDNS is permanently
  // broken but whose cached IP works fine would keep paying the 3s blocking
  // query every RESOLVE_RETRY_MS forever.
  serverIpSuspect = false;
  if (ipFailStreak > 0) {
    logDiag(("fetch_recovered after " + String(ipFailStreak) + " fails").c_str());
    ipFailStreak = 0;
  }

  // Single filtered parse: applyUsageDoc copies into STATE; appendArchiveRow
  // reuses the same doc for today/active_now (no second deserialize).
  JsonDocument doc;
  static JsonDocument filter = usageFilter();
  DeserializationError err = deserializeJson(doc, payload, DeserializationOption::Filter(filter));
  if (err) return false;
  if (!applyUsageDoc(doc, true)) return false;  // live poll: honor power.battery_save

  // Persisting to SD is paced by wall time, not by the poll rate. Poll Interval
  // goes down to 5s, and this block is an SD.remove plus four file writes — at
  // 5s that was ~17k archive rows/day (~4GB/yr) and a constant stream of card
  // I/O competing with the GIF player for sdMutex. None of these files needs
  // finer-than-a-minute granularity: the caches only matter at cold boot, and
  // the archive is for off-device trend analysis. At the 20s/60s/5min poll
  // settings this changes almost nothing; at 5s it cuts card traffic ~12x.
  static uint32_t lastSdPersistMs = 0;
  uint32_t nowMs = millis();
  bool persistDue = (lastSdPersistMs == 0) || (nowMs - lastSdPersistMs >= SD_PERSIST_MIN_MS);
  if (STATE.sdOk && persistDue) {
    lastSdPersistMs = nowMs;
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
