// Claude Code token usage dashboard for the ESP32-2432S028R (CYD).
// Polls a local Python server for usage stats and renders 4 tap-to-cycle pages.

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <SD.h>
#include <AnimatedGIF.h>
#include <esp_system.h>
#include <time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#include "pins.h"
#include "config.h"

// ── DISPLAY CONFIG ────────────────────────────────────────
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9341 _panel_instance;
  lgfx::Bus_SPI _bus_instance;
  lgfx::Light_PWM _light_instance;
  lgfx::Touch_XPT2046 _touch_instance;

public:
  LGFX(void) {
    {
      auto cfg = _bus_instance.config();
      cfg.spi_host = VSPI_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 40000000;
      cfg.freq_read = 16000000;
      cfg.pin_sclk = CYD_TFT_SCLK;
      cfg.pin_mosi = CYD_TFT_MOSI;
      cfg.pin_miso = CYD_TFT_MISO;
      cfg.pin_dc = CYD_TFT_DC;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }
    {
      auto cfg = _panel_instance.config();
      cfg.pin_cs = CYD_TFT_CS;
      cfg.pin_rst = CYD_TFT_RST;
      cfg.panel_width = 240;
      cfg.panel_height = 320;
      cfg.offset_rotation = 0;
      _panel_instance.config(cfg);
    }
    {
      auto cfg = _light_instance.config();
      cfg.pin_bl = CYD_TFT_BL;
      cfg.invert = false;
      _light_instance.config(cfg);
      _panel_instance.setLight(&_light_instance);
    }
    {
      auto cfg = _touch_instance.config();
      cfg.x_min = 200;
      cfg.x_max = 3800;
      // At setRotation(1), LovyanGFX's touch convertRawXY() swaps raw X/Y
      // before scaling — so the screen's left/right axis is actually driven
      // by the RAW Y calibration, not x_min/x_max. Inverted here (rather
      // than x_min/x_max) to match user's board so tapping right advances
      // and tapping left goes back.
      cfg.y_min = 3800;
      cfg.y_max = 200;
      // Touch has its own pins on the CYD — software SPI, not the display bus.
      cfg.spi_host = -1;
      cfg.bus_shared = false;
      cfg.pin_sclk = CYD_TOUCH_SCLK;
      cfg.pin_mosi = CYD_TOUCH_MOSI;
      cfg.pin_miso = CYD_TOUCH_MISO;
      cfg.pin_cs = CYD_TOUCH_CS;
      cfg.pin_int = CYD_TOUCH_IRQ;
      cfg.freq = 1000000;
      _touch_instance.config(cfg);
      _panel_instance.setTouch(&_touch_instance);
    }
    setPanel(&_panel_instance);
  }

  void applyRuntimeConfig(int rotation, int x_min, int x_max, int y_min, int y_max, int offset_rotation) {
    setRotation(rotation);
    auto cfg = _touch_instance.config();
    cfg.x_min = x_min;
    cfg.x_max = x_max;
    cfg.y_min = y_min;
    cfg.y_max = y_max;
    cfg.offset_rotation = offset_rotation;
    _touch_instance.config(cfg);
    _panel_instance.setTouch(&_touch_instance);
  }
};

LGFX gfx;

// The onboard microSD slot has its own dedicated SPI pins, not shared with
// the display (confirmed on hardware — see pins.h) — driven on HSPI so it
// doesn't contend with LovyanGFX's VSPI bus instance.
SPIClass sdSPI(HSPI);

// Off-screen frame buffer: every frame is composed in `frame` and pushed to
// the panel in ONE write, so redraws never flash a blank screen. All drawing
// goes through `g`; if sprite allocation fails at boot we fall back to
// drawing directly on the panel (flickery but functional).
LGFX_Sprite frame(&gfx);
lgfx::LovyanGFX* g = &gfx;

void presentFrame(bool fullScreen = true);

// Touch feedback: flash a 5px white border on the left or right part of the screen
// for one frame, straight to the panel over the already-pushed frame, then re-push
// the clean frame to clear it. Drawn on `gfx` (not `g`) so it overlays the
// composited page; presentFrame() restores the borderless frame.
void flashTouchBorder(bool isRight) {
  const int T = 5;
  const uint16_t WHITE = 0xFFFF;
  if (isRight) {
    gfx.fillRect(160, 0, 160, T, WHITE);        // top right
    gfx.fillRect(160, 240 - T, 160, T, WHITE);  // bottom right
    gfx.fillRect(320 - T, 0, T, 240, WHITE);    // right
  } else {
    gfx.fillRect(0, 0, 160, T, WHITE);          // top left
    gfx.fillRect(0, 240 - T, 160, T, WHITE);    // bottom left
    gfx.fillRect(0, 0, T, 240, WHITE);          // left
  }
  delay(60);
  presentFrame();
}

// ── STATE ──────────────────────────────────────────────────
// Non-const: overridable from /config.json (see loadRuntimeConfig). Set once at
// boot before the two tasks start, then only read, so no lock is needed.
uint32_t POLL_INTERVAL_MS = 20000;
const uint32_t TOUCH_DEBOUNCE_MS = 350;
const int PAGE_COUNT = 7;
const int GIF_PAGE = 5;  // 6th page (0-indexed): random cat GIFs from /cats/ on SD

// Footer pagination dots: right-aligned so the rightmost dot's edge always
// lands at x=319, regardless of PAGE_COUNT (hardcoding a start x broke once
// a 5th dot pushed past the 320px screen edge).
const int DOT_SPACING = 12;
const int DOT_START_X = 300 - (PAGE_COUNT - 1) * DOT_SPACING;

// Total DRAM available to globals+heap on this ESP32 variant/partition
// scheme — a fixed board constant (matches "Maximum is 327680 bytes" in the
// arduino-cli compile report), not something that changes per build.
const uint32_t TOTAL_RAM_BYTES = 327680;

// Bangkok has no DST, so a fixed UTC+7 offset is exact year-round.
const long GMT_OFFSET_SEC = 7 * 3600;
const int DST_OFFSET_SEC = 0;

const uint16_t COL_BG = 0x0841;      // near-black
const uint16_t COL_SURFACE = 0x0841; // card fill = bg (no surface tint)
const uint16_t COL_BORDER = 0x39C7;
const uint16_t COL_TEXT = 0xFFFF;
const uint16_t COL_TEXT2 = 0x9CD3;
const uint16_t COL_ACCENT = 0xFB08;  // orange
const uint16_t COL_GOOD = 0x2668;    // green
const uint16_t COL_WARN = 0xF8C6;    // rose
const uint16_t COL_TRACK = 0x5ACB;   // neutral grey bar track
const uint16_t COL_BLUE = 0x3C1E;    // device-stats bar chart accent

struct UsageState {
  String projectNames[5];
  int64_t projectTokens[5];
  int projectCount = 0;
  int64_t trend[7];
  int sessionPercent = -1;   // -1 = limits unavailable
  String sessionResets;
  long sessionResetsInSec = -1;  // countdown to session reset; -1 = unknown
  int weekPercent = -1;
  String weekResets;
  uint32_t lastFetchOkMs = 0;
  double btcPrice = -1;      // BTC/USDT (from the Mac via /api/usage); -1 = unknown
  float weatherTempC = -999; // Bangkok temp (from the Mac via /api/usage); -999 = unknown
  int weatherCode = -1;      // WMO weather_code (from the Mac); -1 = unknown
  bool sdOk = false;
  int lastLoggedYday = -1;  // tm_yday of the last day appended to the SD log
  bool haveData = false;    // true once any data (live or SD-cached) has been applied
} STATE;

// 30-day on-device history, backed by /daily_log.csv on the SD card so it
// survives independent of the Mac server's log retention/uptime. Kept as
// plain globals (not in UsageState) since they're loaded once at boot and
// shifted in place, not part of the per-poll refresh cycle.
const int LONG_TREND_DAYS = 30;
int64_t longTrend[LONG_TREND_DAYS];
int longTrendCount = 0;  // valid entries, oldest at [0]; may be < LONG_TREND_DAYS early on

int currentPage = 0;
bool mixedPageDirty = false;
uint32_t lastPollMs = 0;
uint32_t lastTouchMs = 0;
int gifMinY = 220;
int gifMaxY = -1;
bool gifFirstFrame = false;

void presentFrame(bool fullScreen) {
  if (g == &frame) {
    // Clear rightmost 16px (5% of 320) to black so it's always blank/padded
    frame.fillRect(304, 0, 16, 240, 0x0000);
    
    bool offline = !STATE.haveData;
    if (currentPage == 6 && !offline) {
      gfx.startWrite();
      if (frame.getColorDepth() == 16) {
        uint16_t* buf = (uint16_t*)frame.getBuffer();
        if (fullScreen) {
          // Push left half (0-159, height 240) + right footer (160-303, rows 220-239)
          for (int y = 0; y < 240; y++) {
            gfx.pushImage(0, y, 160, 1, buf + y * 320);
          }
          for (int y = 220; y < 240; y++) {
            gfx.pushImage(160, y, 144, 1, buf + y * 320 + 160);
          }
        } else {
          // Push only the dirty band of the right half (160-303) where the GIF is drawn
          if (gifMinY <= gifMaxY) {
            int startY = gifMinY < 0 ? 0 : gifMinY;
            int endY = gifMaxY >= 220 ? 219 : gifMaxY;
            for (int y = startY; y <= endY; y++) {
              gfx.pushImage(160, y, 144, 1, buf + y * 320 + 160);
            }
          }
        }
      } else {
        // Fallback for non-16bpp color depths
        uint16_t rowBuf[160];
        if (fullScreen) {
          // Push left half (0-159, height 240)
          for (int y = 0; y < 240; y++) {
            frame.readRect(0, y, 160, 1, rowBuf);
            gfx.pushImage(0, y, 160, 1, rowBuf);
          }
          // Push right footer (160-303, rows 220-239)
          for (int y = 220; y < 240; y++) {
            frame.readRect(160, y, 144, 1, rowBuf);
            gfx.pushImage(160, y, 144, 1, rowBuf);
          }
        } else {
          // Push only the dirty band of the right half (160-303)
          if (gifMinY <= gifMaxY) {
            int startY = gifMinY < 0 ? 0 : gifMinY;
            int endY = gifMaxY >= 220 ? 219 : gifMaxY;
            for (int y = startY; y <= endY; y++) {
              frame.readRect(160, y, 144, 1, rowBuf);
              gfx.pushImage(160, y, 144, 1, rowBuf);
            }
          }
        }
      }
      gfx.endWrite();
    } else {
      frame.pushSprite(0, 0);
    }
  }
}

// CPU load estimate: no FreeRTOS runtime-stats are enabled in the Arduino
// build, so there's no true per-core utilization API. Instead we measure the
// fraction of each loop() iteration spent doing work vs. the fixed 30ms
// delay() at its end, and smooth it with an EMA — a duty-cycle proxy for
// "how busy is the loop", not a real scheduler-level CPU% figure.
float cpuPercentAvg = 0;
bool touchWasDown = false;
bool wifiOk = false;
bool connected = false;  // did the most recent fetch reach the server?

// Guards every read/write of the shared UsageState (and longTrend[]) between
// the render loop (core 1) and the network task (core 0). The network task
// runs its blocking HTTP/TLS fetches WITHOUT holding this lock and only takes
// it for the brief moment it copies parsed results into STATE, so a slow or
// hung fetch can never stall render() or touch handling. render() holds it
// while drawing (STATE reads, including String members) and releases before
// the pushSprite. Non-recursive — draw helpers must never re-lock.
SemaphoreHandle_t stateMutex = nullptr;
inline void lockState()   { if (stateMutex) xSemaphoreTake(stateMutex, portMAX_DELAY); }
inline void unlockState() { if (stateMutex) xSemaphoreGive(stateMutex); }

// Serializes the SD/HSPI bus between the two cores. Normally all SD I/O is on
// networkTask (core 0), but the page-6 GIF player reads frames from SD on the
// render core (core 1) — so both sides must hold this while touching the card.
// NON-RECURSIVE and NON-NESTABLE with stateMutex in the reverse order: the only
// nesting allowed is sdMutex -> stateMutex (as in appendArchiveRow/saveEnvCache
// under fetchUsage's SD block). Nothing takes sdMutex while holding stateMutex,
// so render() (which holds stateMutex) never blocks on the card.
SemaphoreHandle_t sdMutex = nullptr;
inline void lockSD()   { if (sdMutex) xSemaphoreTake(sdMutex, portMAX_DELAY); }
inline void unlockSD() { if (sdMutex) xSemaphoreGive(sdMutex); }

// ── PAGE 6: CAT GIF PLAYER ─────────────────────────────────
// Random cat GIFs from /cats/ on the SD card, played endlessly on the 6th page.
// Decode + draw happen on the render core (core 1) in gifTick(); SD reads there
// are guarded by sdMutex so they can't collide with the network task's writes.
// Allocated only while page 6 is showing (see loop's page-transition handler)
// and freed on exit — its work buffers are ~24KB, and keeping them resident
// permanently starved the mbedTLS handshake that the BTC/weather HTTPS fetches
// need, so those tiles went blank ("--"). Off the GIF page this heap is free.
AnimatedGIF* gif = nullptr;
File gifFile;                 // handle the AnimatedGIF file callbacks read through
const char* CATS_DIR = "/cats";
const int MAX_CATS = 120;     // cap the in-RAM filename list (names are ~18B each)
String catFiles[MAX_CATS];
int catCount = 0;
int gifXOffset = 0, gifYOffset = 0;  // centering offsets for the current GIF
bool gifOpen = false;
bool gifPlaceholderDrawn = false;
uint32_t gifNextFrameMs = 0;

// Server discovery: when SERVER_HOST ends in ".local" we resolve it via mDNS
// (Bonjour) so the board finds the Mac by name even after its IP changes on
// sleep/wake or network reconnect. The resolved IP is cached and re-resolved
// whenever a fetch fails.
IPAddress serverIp;
bool serverIpResolved = false;
bool mdnsStarted = false;
int wifiDownCycles = 0;
const int RESTART_AFTER_CYCLES = 45;  // ~15 min of no WiFi -> self-reboot

// Consecutive failed polls (WiFi down OR WiFi up but the Mac unreachable)
// before STATE.haveData is forced back to false, switching the display to the
// cat-GIF/OFFLINE screen. STATE.haveData is otherwise sticky-true once any
// fetch (or SD cache load) succeeds, so this is what makes a real outage
// actually visible instead of just freezing the last-known dashboard forever.
int fetchFailCycles = 0;
const int OFFLINE_AFTER_CYCLES = 3;  // ~1 min at the default 20s poll interval

// Runtime overrides for the compiled config.h defaults, loaded from an
// optional /config.json on the SD card (see loadRuntimeConfig()) so WiFi
// creds or the Mac's host/port can change without reflashing.
String cfgWifiSsid = WIFI_SSID;
String cfgWifiPassword = WIFI_PASSWORD;
String cfgServerHost = SERVER_HOST;
int cfgServerPort = SERVER_PORT;
int cfgBrightness = 200;   // 0-255 panel backlight; overridable via /config.json
int cfgScreenRotation = 1;
int cfgTouchXMin = 200;
int cfgTouchXMax = 3800;
int cfgTouchYMin = 3800;   // inverted on user's board (drives left/right at rotation 1)
int cfgTouchYMax = 200;
int cfgTouchOffsetRotation = 0;

// ── FORMATTING ─────────────────────────────────────────────
String fmtTokens(int64_t t) {
  if (t >= 1000000000) return String((double)t / 1000000000.0, 2) + "B";
  if (t >= 1000000) return String((double)t / 1000000.0, 1) + "M";
  if (t >= 1000) return String((double)t / 1000.0, 1) + "K";
  return String((long)t);
}

String fmtCost(float c) {
  return "$" + String(c, 2);
}

String fmtBtc(double p) {
  if (p < 0) return "--";
  String s = String((long)(p + 0.5));
  String out;
  int len = s.length();
  for (int i = 0; i < len; i++) {
    out += s[i];
    int rem = len - 1 - i;
    if (rem > 0 && rem % 3 == 0) out += ',';
  }
  return out;
}

String fmtCountdown(long sec) {
  if (sec < 0) return "";
  long m = (sec + 30) / 60;  // round to nearest minute
  long h = m / 60;
  m %= 60;
  char buf[16];
  if (h > 0) snprintf(buf, sizeof(buf), "%ldh:%02ldm", h, m);
  else snprintf(buf, sizeof(buf), "%ldm", m);
  return String(buf);
}

String fmtKB(uint32_t bytes) {
  return String((float)bytes / 1024.0f, 1) + "KB";
}

// Flash used by the sketch as a % of the OTA app partition — genuinely
// queried from the running device, not a build-log constant.
int flashPercent(uint32_t &usedOut, uint32_t &totalOut) {
  uint32_t used = ESP.getSketchSize();
  uint32_t total = used + ESP.getFreeSketchSpace();
  usedOut = used;
  totalOut = total;
  return total ? (int)((float)used / total * 100 + 0.5f) : -1;
}

// Static (global/.bss) RAM as a % of total DRAM: TOTAL_RAM_BYTES minus the
// heap capacity the runtime reports (ESP.getHeapSize() already excludes
// whatever's reserved for statics), queried live rather than hardcoded.
int staticRamPercent(uint32_t &usedOut) {
  uint32_t heapCapacity = ESP.getHeapSize();
  uint32_t used = heapCapacity < TOTAL_RAM_BYTES ? TOTAL_RAM_BYTES - heapCapacity : 0;
  usedOut = used;
  return (int)((float)used / TOTAL_RAM_BYTES * 100 + 0.5f);
}

String fmtGB(uint64_t bytes) {
  return String((float)bytes / (1024.0f * 1024.0f * 1024.0f), 1) + "GB";
}

// SD card space in use, queried live each time the page renders — cheap FAT
// bookkeeping reads, not a scan of the card. -1 (with usedOut/totalOut
// untouched) when the card never mounted.
int sdCapacityPercent(uint64_t &usedOut, uint64_t &totalOut) {
  if (!STATE.sdOk) return -1;
  uint64_t total = SD.totalBytes();
  uint64_t used = SD.usedBytes();
  usedOut = used;
  totalOut = total;
  return total ? (int)((float)used / total * 100 + 0.5f) : -1;
}

// ── NETWORK ────────────────────────────────────────────────
// (Re)start the mDNS resolver whenever WiFi comes up — needed both after the
// first connect and after any reconnect (the board's own IP may have changed).
void ensureMdns() {
  if (!mdnsStarted && MDNS.begin("cyd-dashboard")) {
    mdnsStarted = true;
  }
}

// 8 dots arranged in a circle, one lit at a time to give a rotating-spinner
// effect while connectWifi() blocks waiting for an AP.
const int SPINNER_DOTS = 8;

void drawWifiSpinner(int frame) {
  const int cx = 152, cy = 120, r = 24, dotR = 4;
  g->fillScreen(COL_BG);
  for (int i = 0; i < SPINNER_DOTS; i++) {
    float angle = i * 2 * PI / SPINNER_DOTS;
    int x = cx + (int)(r * cosf(angle));
    int y = cy + (int)(r * sinf(angle));
    g->fillCircle(x, y, dotR, i == frame % SPINNER_DOTS ? COL_ACCENT : COL_BORDER);
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
  int frame = 0;
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    drawWifiSpinner(frame++);
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

const char* CACHE_PATH = "/last_usage.json";

// Parses one /api/usage payload into STATE. Shared by a live fetch and a
// cached-on-SD read, so a stale card-backed copy renders identically to a
// fresh one.
bool applyUsageJson(const String& payload) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) return false;

  // Parse above runs on a local doc (no shared state); take the lock only for
  // the copy into STATE below, which the render loop reads concurrently.
  lockState();
  JsonArray projects = doc["projects"].as<JsonArray>();
  STATE.projectCount = 0;
  for (JsonObject p : projects) {
    if (STATE.projectCount >= 5) break;
    STATE.projectNames[STATE.projectCount] = p["name"].as<String>();
    STATE.projectTokens[STATE.projectCount] = p["tokens"] | (int64_t)0;
    STATE.projectCount++;
  }

  JsonArray trend = doc["trend"].as<JsonArray>();
  for (int i = 0; i < 7; i++) {
    STATE.trend[i] = i < (int)trend.size() ? trend[i].as<int64_t>() : 0;
  }

  if (!doc["limits"].isNull()) {
    STATE.sessionPercent = doc["limits"]["session"]["percent"] | -1;
    STATE.sessionResets = doc["limits"]["session"]["resets"].as<String>();
    STATE.sessionResetsInSec = doc["limits"]["session"]["resets_in_sec"] | -1L;
    STATE.weekPercent = doc["limits"]["week"]["percent"] | -1;
    STATE.weekResets = doc["limits"]["week"]["resets"].as<String>();
  } else {
    STATE.sessionPercent = -1;
    STATE.sessionResets = "";
    STATE.sessionResetsInSec = -1L;
    STATE.weekPercent = -1;
    STATE.weekResets = "";
  }

  // BTC + weather are fetched by the Mac and delivered in this same payload (the
  // board can't do TLS — see the server's market_loop). Keep the last known
  // value if a field is absent so the tiles don't flicker to "--".
  if (!doc["btc"].isNull()) {
    double p = doc["btc"]["price"] | -1.0;
    if (p > 0) STATE.btcPrice = p;
  }
  if (!doc["weather"].isNull()) {
    STATE.weatherTempC = doc["weather"]["tempC"] | STATE.weatherTempC;
    STATE.weatherCode = doc["weather"]["code"] | STATE.weatherCode;
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

const char* ENV_CACHE_PATH = "/last_env.json";
const char* ARCHIVE_PATH = "/archive.csv";

// Persist the latest BTC price + Bangkok weather so those tiles show
// last-known values immediately on a cold boot (and while offline) instead of
// "--" until the first live fetch lands. Written once per usage poll (~20s) to
// bound card wear, not on every 5s BTC refresh.
void saveEnvCache() {
  if (!STATE.sdOk) return;
  lockState();
  double btc = STATE.btcPrice;
  float tempC = STATE.weatherTempC;
  int code = STATE.weatherCode;
  unlockState();
  SD.remove(ENV_CACHE_PATH);  // FILE_WRITE appends here; remove for a clean overwrite
  File f = SD.open(ENV_CACHE_PATH, FILE_WRITE);
  if (f) {
    f.printf("{\"btc\":%.2f,\"tempC\":%.1f,\"code\":%d}", btc, tempC, code);
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
  unlockState();
}

// Fine-grained, append-only history — one row per successful poll (~20s),
// unbounded (that's the point of the roomy card; ~1GB/year). Meant for
// off-device analysis, not shown on screen — distinct from /daily_log.csv,
// which keeps only one end-of-day row for the on-screen 30-day trend.
void appendArchiveRow(const String& payload) {
  if (!STATE.sdOk) return;

  struct tm timeinfo;
  char tsBuf[24];
  if (!getLocalTime(&timeinfo, 0)) return;  // no wall-clock yet — skip, don't write a bogus ts
  snprintf(tsBuf, sizeof(tsBuf), "%04d-%02d-%02d %02d:%02d:%02d",
           timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
           timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);

  JsonDocument doc;
  if (deserializeJson(doc, payload)) return;
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
    appendArchiveRow(payload);  // fine-grained per-poll history for later analysis
    saveEnvCache();             // refresh the BTC/weather cold-boot cache
    unlockSD();
  }

  return true;
}

// BTC price and Bangkok weather are NOT fetched on the board. This unit has no
// PSRAM, and the 154KB framebuffer leaves too little contiguous heap for an
// mbedTLS handshake (verified: TCP 443 connects but the TLS buffer allocation
// fails). Instead the Mac fetches both and includes them in /api/usage, which
// the board reads over plain HTTP; applyUsageJson() copies them into STATE.

// ── SD CONFIG / DIAGNOSTICS ────────────────────────────────
// Optional override for the compiled config.h defaults. Missing file or
// parse failure is silent — it just means "use the compiled defaults",
// not an error worth surfacing.
void loadRuntimeConfig() {
  File f = SD.open("/config.json", FILE_READ);
  if (!f) return;
  String payload = f.readString();
  f.close();

  JsonDocument doc;
  if (deserializeJson(doc, payload)) return;
  if (!doc["wifi_ssid"].isNull()) cfgWifiSsid = doc["wifi_ssid"].as<String>();
  if (!doc["wifi_password"].isNull()) cfgWifiPassword = doc["wifi_password"].as<String>();
  if (!doc["server_host"].isNull()) cfgServerHost = doc["server_host"].as<String>();
  if (!doc["server_port"].isNull()) cfgServerPort = doc["server_port"].as<int>();
  if (!doc["brightness"].isNull()) {
    cfgBrightness = constrain(doc["brightness"].as<int>(), 0, 255);
  }
  if (!doc["poll_interval_sec"].isNull()) {
    // Clamp to a sane floor so a typo can't hammer the Mac or the card.
    int sec = doc["poll_interval_sec"].as<int>();
    POLL_INTERVAL_MS = (uint32_t)constrain(sec, 5, 3600) * 1000;
  }
  if (!doc["screen_rotation"].isNull()) cfgScreenRotation = doc["screen_rotation"].as<int>();
  if (!doc["touch_x_min"].isNull()) cfgTouchXMin = doc["touch_x_min"].as<int>();
  if (!doc["touch_x_max"].isNull()) cfgTouchXMax = doc["touch_x_max"].as<int>();
  if (!doc["touch_y_min"].isNull()) cfgTouchYMin = doc["touch_y_min"].as<int>();
  if (!doc["touch_y_max"].isNull()) cfgTouchYMax = doc["touch_y_max"].as<int>();
  if (!doc["touch_offset_rotation"].isNull()) cfgTouchOffsetRotation = doc["touch_offset_rotation"].as<int>();
  Serial.println("[config] loaded overrides from /config.json");
}

const char* resetReasonStr() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "POWERON";
    case ESP_RST_SW: return "SW";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT: return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    default: return "UNKNOWN";
  }
}

// Appends one timestamped event to /diag_log.csv — meant to be read later
// off the card, not surfaced on screen. Never blocks retrying: a failed
// write is dropped, not queued.
void logDiag(const char* event) {
  if (!STATE.sdOk) return;

  struct tm timeinfo;
  char tsBuf[24];
  if (getLocalTime(&timeinfo, 0)) {
    snprintf(tsBuf, sizeof(tsBuf), "%04d-%02d-%02d %02d:%02d:%02d",
              timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
              timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
  } else {
    snprintf(tsBuf, sizeof(tsBuf), "boot+%lus", (unsigned long)(millis() / 1000));
  }

  lockSD();
  File f = SD.open("/diag_log.csv", FILE_APPEND);
  if (f) {
    f.printf("%s,%s\n", tsBuf, event);
    f.close();
  }
  unlockSD();
}

// ── SD LOG ─────────────────────────────────────────────────
const char* DAILY_LOG_PATH = "/daily_log.csv";

// Read the tail of the CSV into longTrend[]. Streamed line-by-line (never
// buffered whole) since the file grows unbounded over the card's lifetime;
// only the most recent LONG_TREND_DAYS rows matter.
void loadLongTrendFromSD() {
  longTrendCount = 0;
  File f = SD.open(DAILY_LOG_PATH, FILE_READ);
  if (!f) return;  // no log yet (fresh card) — not an error

  String lastDate;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (!line.length()) continue;
    int comma = line.indexOf(',');
    if (comma < 0) continue;
    lastDate = line.substring(0, comma);
    int64_t tokens = line.substring(comma + 1).toInt();
    if (longTrendCount < LONG_TREND_DAYS) {
      longTrend[longTrendCount++] = tokens;
    } else {
      // Shift left to keep only the newest LONG_TREND_DAYS rows.
      memmove(longTrend, longTrend + 1, (LONG_TREND_DAYS - 1) * sizeof(int64_t));
      longTrend[LONG_TREND_DAYS - 1] = tokens;
    }
  }
  f.close();

  // Recover lastLoggedYday from the last logged date so a reboot mid-day
  // doesn't re-append a row for a day already recorded.
  if (lastDate.length() == 10) {
    struct tm t = {};
    t.tm_year = lastDate.substring(0, 4).toInt() - 1900;
    t.tm_mon = lastDate.substring(5, 7).toInt() - 1;
    t.tm_mday = lastDate.substring(8, 10).toInt();
    time_t asTime = mktime(&t);
    struct tm normalized;
    localtime_r(&asTime, &normalized);
    STATE.lastLoggedYday = normalized.tm_yday;
  }
}

// Called once per poll cycle, right after fetchUsage(). Appends a row only
// when the local calendar day has just rolled over since the last poll,
// logging the day that just ended (yesterday, relative to nowInfo) with the
// token total it finished on — captured by the caller *before* fetchUsage()
// overwrote STATE.trend[6] with the new day's (near-zero) running total.
void appendDailyLogIfNeeded(const struct tm& nowInfo, int64_t justEndedDayTokens) {
  if (!STATE.sdOk) return;

  struct tm nowCopy = nowInfo;
  time_t nowEpoch = mktime(&nowCopy);
  time_t yesterdayEpoch = nowEpoch - 86400;
  struct tm yesterday;
  localtime_r(&yesterdayEpoch, &yesterday);

  lockState();
  int lastYday = STATE.lastLoggedYday;
  unlockState();

  if (yesterday.tm_yday == lastYday) return;  // same day, nothing to do

  if (lastYday == -1) {
    // First poll ever (fresh card, nothing to compare against yet) — just
    // record yesterday's yday as the baseline; the actual log starts tomorrow.
    lockState();
    STATE.lastLoggedYday = yesterday.tm_yday;
    unlockState();
    return;
  }

  char dateBuf[11];
  snprintf(dateBuf, sizeof(dateBuf), "%04d-%02d-%02d",
            yesterday.tm_year + 1900, yesterday.tm_mon + 1, yesterday.tm_mday);

  lockSD();
  File f = SD.open(DAILY_LOG_PATH, FILE_APPEND);
  if (f) {
    f.printf("%s,%lld\n", dateBuf, (long long)justEndedDayTokens);
    f.close();
  }
  unlockSD();

  lockState();
  if (longTrendCount < LONG_TREND_DAYS) {
    longTrend[longTrendCount++] = justEndedDayTokens;
  } else {
    memmove(longTrend, longTrend + 1, (LONG_TREND_DAYS - 1) * sizeof(int64_t));
    longTrend[LONG_TREND_DAYS - 1] = justEndedDayTokens;
  }
  STATE.lastLoggedYday = yesterday.tm_yday;
  unlockState();
}

// ── SELF-HOSTED ASSETS ─────────────────────────────────────
// Minimal 24-bit uncompressed BMP loader so image assets can live on the SD
// card instead of being baked into flash (which is already ~95% full). BMP rows
// are bottom-up unless height is negative; each row is read into a small static
// buffer, converted to RGB565, and blitted straight to the panel. Returns false
// (silently) for a missing file or any unsupported format — callers treat the
// asset as simply absent.
bool drawBmpFromSD(const char* path, int dx, int dy) {
  if (!STATE.sdOk) return false;
  File f = SD.open(path, FILE_READ);
  if (!f) return false;

  uint8_t hdr[54];
  if (f.read(hdr, 54) != 54 || hdr[0] != 'B' || hdr[1] != 'M') { f.close(); return false; }
  uint32_t dataOffset = hdr[10] | (hdr[11] << 8) | (hdr[12] << 16) | ((uint32_t)hdr[13] << 24);
  int32_t  width  = hdr[18] | (hdr[19] << 8) | (hdr[20] << 16) | ((uint32_t)hdr[21] << 24);
  int32_t  height = hdr[22] | (hdr[23] << 8) | (hdr[24] << 16) | ((uint32_t)hdr[25] << 24);
  uint16_t bpp    = hdr[28] | (hdr[29] << 8);
  if (bpp != 24 || width <= 0 || width > 320 || height == 0 || abs(height) > 240) {
    f.close();
    return false;
  }

  bool topDown = height < 0;
  int32_t h = topDown ? -height : height;
  int rowSize = (width * 3 + 3) & ~3;  // BMP rows are padded to a 4-byte boundary

  static uint8_t rowBuf[320 * 3];
  static uint16_t pxBuf[320];
  for (int32_t r = 0; r < h; r++) {
    int32_t srcRow = topDown ? r : (h - 1 - r);
    f.seek(dataOffset + (uint32_t)srcRow * rowSize);
    if (f.read(rowBuf, rowSize) != rowSize) break;
    for (int32_t x = 0; x < width; x++) {
      uint8_t b  = rowBuf[x * 3];
      uint8_t g8 = rowBuf[x * 3 + 1];
      uint8_t rr = rowBuf[x * 3 + 2];
      pxBuf[x] = ((rr & 0xF8) << 8) | ((g8 & 0xFC) << 3) | (b >> 3);
    }
    gfx.pushImage(dx, dy + r, width, 1, pxBuf);
  }
  f.close();
  return true;
}

// Optional boot splash: if the user dropped a 320x240 24-bit /splash.bmp on the
// card, show it briefly before the WiFi spinner. Absent file -> no splash, no
// delay, straight into the normal boot.
void showBootSplash() {
  if (drawBmpFromSD("/splash.bmp", 0, 0)) delay(1500);
}

// ── DRAWING HELPERS ────────────────────────────────────────
void drawFooter() {
  // Status dot: pulses once a second while the most recent poll reached the
  // server — big+green on the odd second, small+red on the even second, so the
  // pulse is easy to read at a glance. Static amber when coasting on cached
  // data (haveData true but connected false — render() only gets here when
  // haveData is true, so there's no "no data" case left to color for).
  bool bigPhase = (millis() / 1000) % 2;
  int dotR = (connected && bigPhase) ? 4 : 3;
  uint16_t dotColor = !connected ? COL_ACCENT : (bigPhase ? COL_GOOD : COL_WARN);
  g->fillCircle(14, 230, dotR, dotColor);

  for (int i = 0; i < PAGE_COUNT; i++) {
    uint16_t c = (i == currentPage) ? COL_ACCENT : COL_BORDER;
    g->fillCircle(DOT_START_X + i * DOT_SPACING, 230, 3, c);
  }

  uint32_t flashUsed, flashTotal, ramUsed;
  int romPct = flashPercent(flashUsed, flashTotal);
  int ramPct = staticRamPercent(ramUsed);
  int cpuInt = (int)(cpuPercentAvg + 0.5f);
  
  g->setTextSize(1);
  g->setCursor(30, 226);
  
  g->setTextColor(COL_TEXT2);
  g->print("CPU ");
  g->setTextColor(COL_TEXT);
  g->print(String(cpuInt) + "%");
  g->setTextColor(COL_TEXT2);
  g->print("  ROM ");
  g->setTextColor(COL_TEXT);
  g->print(String(romPct) + "%");
  g->setTextColor(COL_TEXT2);
  g->print("  RAM ");
  g->setTextColor(COL_TEXT);
  g->print(String(ramPct) + "%");

  // Minimal 1px line along the bottom edge, filling left-to-right as the
  // next poll approaches (full width = fetch imminent).
  uint32_t elapsed = millis() - lastPollMs;
  if (elapsed > POLL_INTERVAL_MS) elapsed = POLL_INTERVAL_MS;
  int lineW = (int)((float)elapsed / POLL_INTERVAL_MS * 304);
  if (lineW > 0) g->fillRect(0, 239, lineW, 1, COL_TEXT2);
}

void drawLimitBlock(int y, const char* title, int percent, const String& resets, long resetsInSec) {
  g->setTextColor(COL_TEXT);
  g->setTextSize(1);
  g->setCursor(10, y);
  g->print(title);

  int barX = 10, barY = y + 22, barW = 175, barH = 16;
  g->fillRoundRect(barX, barY, barW, barH, 3, COL_TRACK);
  if (percent >= 0) {
    int fillW = (int)((float)min(percent, 100) / 100 * barW);
    g->fillRoundRect(barX, barY, max(fillW, 3), barH, 3, COL_ACCENT);
  }

  g->setTextColor(COL_TEXT);
  g->setTextSize(2);
  g->setCursor(195, barY + 1);
  g->print(percent >= 0 ? String(percent) + "% used" : "--");

  g->setTextColor(COL_TEXT2);
  g->setTextSize(2);
  g->setCursor(10, barY + 26);
  String line = resets.length() ? "Resets " + resets : "Resets --";
  if (resets.length() && resetsInSec >= 0) {
    line += " (" + fmtCountdown(resetsInSec) + ")";
  }
  g->print(line);
}

void drawHomePage() {
  // Tick the session countdown down locally between polls.
  long sessionRem = STATE.sessionResetsInSec;
  if (sessionRem >= 0) {
    sessionRem -= (long)((millis() - STATE.lastFetchOkMs) / 1000);
    if (sessionRem < 0) sessionRem = 0;
  }
  drawLimitBlock(10, "Current session", STATE.sessionPercent, STATE.sessionResets, sessionRem);
  drawLimitBlock(104, "Current week (all models)", STATE.weekPercent, STATE.weekResets, -1);

  // ₿ has no glyph in the built-in ASCII font: print "B" and add the two
  // vertical strokes piercing its top and bottom (size-2 glyph = 10x14 px).
  g->setTextColor(COL_TEXT);
  g->setTextSize(2);
  g->setCursor(10, 198);
  g->print("B");
  g->fillRect(12, 195, 2, 3, COL_TEXT);
  g->fillRect(16, 195, 2, 3, COL_TEXT);
  g->fillRect(12, 212, 2, 3, COL_TEXT);
  g->fillRect(16, 212, 2, 3, COL_TEXT);
  g->print(" $" + fmtBtc(STATE.btcPrice));
}

// Combined page: top 4 projects (7d) in the upper half, the 7-day token trend
// chart in the lower half (formerly its own page). A 1px divider at y=128
// splits the two sections; the trend labels end at y=226, just above the
// footer dots at y=230.
void drawProjectsPage() {
  // ── upper half: top projects (7d) ──
  g->setTextColor(COL_TEXT);
  g->setTextSize(1);
  g->setCursor(10, 6);
  g->print("TOP PROJECTS (7D)");

  if (STATE.projectCount == 0) {
    g->setTextColor(COL_TEXT2);
    g->setCursor(10, 24);
    g->print("No data yet");
  } else {
    int shown = STATE.projectCount < 4 ? STATE.projectCount : 4;
    int64_t maxTokens = 1;
    for (int i = 0; i < shown; i++) {
      if (STATE.projectTokens[i] > maxTokens) maxTokens = STATE.projectTokens[i];
    }

    int y = 22;
    int barMaxW = 230;
    for (int i = 0; i < shown; i++) {
      g->setTextColor(COL_TEXT);
      g->setTextSize(1);
      g->setCursor(10, y);
      g->print(STATE.projectNames[i]);

      int barW = (int)((float)STATE.projectTokens[i] / maxTokens * barMaxW);
      g->fillRoundRect(10, y + 10, barMaxW, 8, 2, COL_SURFACE);
      g->fillRoundRect(10, y + 10, max(barW, 4), 8, 2, COL_ACCENT);

      g->setTextColor(COL_TEXT2);
      g->setCursor(10 + barMaxW + 8, y + 11);
      g->print(fmtTokens(STATE.projectTokens[i]));

      y += 26;
    }
  }

  // ── lower half: 7-day trend ──
  g->fillRect(10, 128, 284, 1, COL_BORDER);
  g->setTextColor(COL_TEXT);
  g->setTextSize(1);
  g->setCursor(10, 134);
  g->print("7-DAY TREND");

  int64_t maxTrend = 1;
  for (int i = 0; i < 7; i++) {
    if (STATE.trend[i] > maxTrend) maxTrend = STATE.trend[i];
  }

  const char* dayLabels[7] = {"-6", "-5", "-4", "-3", "-2", "-1", "today"};
  int chartX = 24, chartY = 150, chartH = 64, barW = 32, gap = 8;
  for (int i = 0; i < 7; i++) {
    int barH = (int)((float)STATE.trend[i] / maxTrend * chartH);
    int bx = chartX + i * (barW + gap);
    int by = chartY + chartH - barH;
    g->fillRoundRect(bx, by, barW, max(barH, 2), 3, COL_ACCENT);
    g->setTextColor(COL_TEXT2);
    g->setTextSize(1);
    g->setCursor(bx, chartY + chartH + 4);
    g->print(dayLabels[i]);
  }
}

// Simple vector weather icon (the built-in font has no bitmap glyphs) drawn
// from circles/rects, mapped from Open-Meteo's WMO weather_code. cx,cy is
// the center of a ~24x20px cloud/sun anchored at (x,y).
void drawWeatherIcon(int x, int y, int code) {
  if (code < 0) {
    g->setTextColor(COL_TEXT2);
    g->setTextSize(1);
    g->setCursor(x, y + 6);
    g->print("--");
    return;
  }
  int cx = x + 10, cy = y + 10;
  if (code == 0 || code == 1) {
    // clear: sun with 4 rays
    g->fillCircle(cx, cy, 6, COL_ACCENT);
    g->fillRect(cx - 1, y - 2, 2, 4, COL_ACCENT);
    g->fillRect(cx - 1, y + 18, 2, 4, COL_ACCENT);
    g->fillRect(x - 2, cy - 1, 4, 2, COL_ACCENT);
    g->fillRect(x + 20, cy - 1, 4, 2, COL_ACCENT);
    return;
  }
  // Everything else shares a cloud base (2/3/45/48 = plain cloudy/fog).
  g->fillCircle(cx - 5, cy - 1, 5, COL_TEXT2);
  g->fillCircle(cx + 2, cy - 3, 6, COL_TEXT2);
  g->fillCircle(cx + 8, cy - 1, 5, COL_TEXT2);
  g->fillRoundRect(cx - 9, cy - 1, 24, 7, 3, COL_TEXT2);
  if (code >= 95) {
    // thunderstorm: bolt
    g->fillRect(cx, cy + 7, 3, 6, COL_ACCENT);
  } else if ((code >= 71 && code <= 77) || code == 85 || code == 86) {
    // snow: dots
    g->fillCircle(cx - 4, cy + 10, 1, COL_TEXT);
    g->fillCircle(cx + 2, cy + 10, 1, COL_TEXT);
    g->fillCircle(cx + 8, cy + 10, 1, COL_TEXT);
  } else if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) {
    // rain: streaks
    g->fillRect(cx - 4, cy + 7, 2, 6, COL_BLUE);
    g->fillRect(cx + 2, cy + 7, 2, 6, COL_BLUE);
    g->fillRect(cx + 8, cy + 7, 2, 6, COL_BLUE);
  }
}

// Thin inset progress bar: near-black track recessed into a surface card.
void drawMiniBar(int x, int y, int w, int percent, uint16_t color) {
  g->fillRoundRect(x, y, w, 6, 3, COL_TRACK);
  if (percent >= 0) {
    int fillW = (int)((float)min(percent, 100) / 100 * w);
    g->fillRoundRect(x, y, max(fillW, 6), 6, 3, color);
  }
}

// Uppercase section label.
void drawCardLabel(int x, int y, const char* label) {
  g->setTextColor(COL_TEXT2);
  g->setTextSize(1);
  g->setCursor(x, y);
  g->print(label);
}

void drawLimitsCard() {
  g->fillRoundRect(2, 4, 142, 216, 8, COL_SURFACE);
  g->drawRoundRect(2, 4, 142, 216, 8, COL_BORDER);

  // ── left card: limits ──
  drawCardLabel(12, 13, "SESSION (5H)");

  // Tick the session countdown down locally between polls.
  long sessionRem = STATE.sessionResetsInSec;
  if (sessionRem >= 0) {
    sessionRem -= (long)((millis() - STATE.lastFetchOkMs) / 1000);
    if (sessionRem < 0) sessionRem = 0;
  }

  g->setTextColor(COL_ACCENT);
  g->setTextSize(4);
  g->setCursor(12, 28);
  g->print(STATE.sessionPercent >= 0 ? String(STATE.sessionPercent) + "%" : "--");

  drawMiniBar(12, 64, 122, STATE.sessionPercent, COL_ACCENT);

  g->setTextColor(COL_TEXT2);
  g->setTextSize(2);
  g->setCursor(12, 76);
  g->print("resets:");
  g->setCursor(12, 94);
  g->print(STATE.sessionResets.length() ? STATE.sessionResets : "--");
  g->setTextColor(COL_TEXT);
  g->setTextSize(2);
  g->setCursor(12, 112);
  if (sessionRem >= 0) g->print("in " + fmtCountdown(sessionRem));

  g->fillRect(12, 136, 122, 1, COL_BORDER);

  drawCardLabel(12, 147, "WEEK (ALL MODELS)");

  g->setTextColor(COL_ACCENT);
  g->setTextSize(3);
  g->setCursor(12, 162);
  g->print(STATE.weekPercent >= 0 ? String(STATE.weekPercent) + "%" : "--");

  drawMiniBar(12, 192, 122, STATE.weekPercent, COL_ACCENT);

  // "resets Jul 9 at 4:59am" is 22 chars = 132px at size1 — exactly the
  // card's inner width, so this line must stay size1.
  g->setTextColor(COL_TEXT2);
  g->setTextSize(1);
  g->setCursor(12, 206);
  g->print(STATE.weekResets.length() ? "resets " + STATE.weekResets : "resets --");
}

void drawMixedPageStatic() {
  // Clear the footer background area to prevent text bloating/overlapping
  g->fillRect(0, 220, 320, 20, COL_BG);
  drawLimitsCard();
  drawFooter();
}

// Card layout: one tall left card = session/week limits (always accent
// orange); the right column is three stacked cards = Bangkok clock/date
// (y=4 h=92), weather (y=100 h=56), BTC (y=160 h=56), 4px gaps. All cards
// are 152px wide with 10px inner padding (content x=14 / x=174, width 132).
void drawStatusPage() {
  drawLimitsCard();
  g->fillRoundRect(150, 4, 150, 92, 8, COL_SURFACE);
  g->drawRoundRect(150, 4, 150, 92, 8, COL_BORDER);
  g->fillRoundRect(150, 100, 150, 56, 8, COL_SURFACE);
  g->drawRoundRect(150, 100, 150, 56, 8, COL_BORDER);
  g->fillRoundRect(150, 160, 150, 56, 8, COL_SURFACE);
  g->drawRoundRect(150, 160, 150, 56, 8, COL_BORDER);

  // ── right cards: Bangkok clock/date, weather, BTC price ──
  struct tm timeinfo;
  bool haveTime = getLocalTime(&timeinfo, 0);

  drawCardLabel(160, 13, "BANGKOK");

  g->setTextSize(4);
  g->setCursor(160, 28);
  if (haveTime) {
    char hm[8];
    snprintf(hm, sizeof(hm), "%02d.%02d", timeinfo.tm_hour, timeinfo.tm_min);
    g->setTextColor(COL_TEXT);
    g->print(hm);
    char ss[4];
    snprintf(ss, sizeof(ss), ".%02d", timeinfo.tm_sec);
    g->setTextColor(COL_TEXT2);
    g->setTextSize(1);
    // Bottom-align the small seconds against the big "HH.MM" cell (32px
    // tall at size4 vs. 8px at size1): x is the fixed 5-char size4 advance.
    g->setCursor(160 + 5 * 24, 52);
    g->print(ss);
  } else {
    g->setTextColor(COL_TEXT2);
    g->print("--.--");
  }

  g->setTextColor(COL_TEXT2);
  g->setTextSize(2);
  g->setCursor(160, 66);
  if (haveTime) {
    // Year dropped: "Mon 25 Jul 2026" at size2 would overrun the 132px
    // inner width; weekday+day+month fits at 120px.
    static const char* wdays[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char* mons[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char buf[16];
    snprintf(buf, sizeof(buf), "%s %d %s", wdays[timeinfo.tm_wday], timeinfo.tm_mday,
             mons[timeinfo.tm_mon]);
    g->print(buf);
  } else {
    g->print("--");
  }

  drawCardLabel(160, 109, "WEATHER");

  drawWeatherIcon(160, 124, STATE.weatherCode);
  g->setTextColor(COL_TEXT);
  g->setTextSize(3);
  g->setCursor(190, 122);
  // No degree glyph in the built-in ASCII font, so just suffix "C".
  g->print(STATE.weatherTempC > -900 ? String((int)round(STATE.weatherTempC)) + "C" : "--");

  drawCardLabel(160, 169, "BTC/USDT");

  g->setTextColor(COL_TEXT);
  g->setTextSize(3);
  g->setCursor(156, 182);
  g->print(fmtBtc(STATE.btcPrice));
}

void drawLongTrendPage() {
  g->setTextColor(COL_TEXT);
  g->setTextSize(2);
  g->setCursor(10, 6);
  g->print("30-Day Trend");

  if (!STATE.sdOk) {
    // A bare centered grey message, since this page has nothing to show
    // without the card.
    g->setTextColor(COL_TEXT2);
    g->setTextSize(2);
    g->setCursor(52, 110);  // centered: "SD CARD NOT FOUND" is 17 chars * 12px = 204px
    g->print("SD CARD NOT FOUND");
    return;
  }

  // longTrend[] only gets a row once a day rolls over (appendDailyLogIfNeeded),
  // so on its own it stops at yesterday — append today's still-running total
  // as one more bar so the rightmost bar actually matches the "today" label.
  int64_t bars[LONG_TREND_DAYS];
  int barCount = longTrendCount;
  for (int i = 0; i < barCount; i++) bars[i] = longTrend[i];
  if (barCount < LONG_TREND_DAYS) {
    bars[barCount++] = STATE.trend[6];
  } else {
    memmove(bars, bars + 1, (LONG_TREND_DAYS - 1) * sizeof(int64_t));
    bars[LONG_TREND_DAYS - 1] = STATE.trend[6];
  }

  int64_t maxTokens = 1;
  for (int i = 0; i < barCount; i++) {
    if (bars[i] > maxTokens) maxTokens = bars[i];
  }

  int chartX = 10, chartY = 36, chartH = 148, chartW = 290;
  int barW = 6, gap = 4;
  int startX = chartX + chartW - barCount * (barW + gap);
  for (int i = 0; i < barCount; i++) {
    int barH = (int)((float)bars[i] / maxTokens * chartH);
    int bx = startX + i * (barW + gap);
    int by = chartY + chartH - barH;
    g->fillRoundRect(bx, by, barW, max(barH, 2), 2, COL_ACCENT);
  }

  g->setTextColor(COL_TEXT2);
  g->setTextSize(1);
  g->setCursor(chartX, chartY + chartH + 6);
  g->print("oldest");
  g->setCursor(chartX + chartW - 30, chartY + chartH + 6);
  g->print("today");
  g->setCursor(chartX, chartY + chartH + 18);
  g->print(String(longTrendCount) + " days logged, peak " + fmtTokens(maxTokens));
}

void drawFullStatBlock(int y, const char* title, int percent, const String& sub, uint16_t color) {
  g->setTextColor(COL_TEXT2);
  g->setTextSize(1);
  g->setCursor(10, y);
  g->print(title);

  int barX = 10, barY = y + 14, barW = 230, barH = 16;
  g->fillRoundRect(barX, barY, barW, barH, 3, COL_TRACK);
  if (percent >= 0) {
    int fillW = (int)((float)min(percent, 100) / 100 * barW);
    g->fillRoundRect(barX, barY, max(fillW, 3), barH, 3, color);
  }

  g->setTextColor(COL_TEXT);
  g->setTextSize(2);
  g->setCursor(barX + barW + 8, barY);
  g->print(percent >= 0 ? String(percent) + "%" : "--");

  g->setTextColor(COL_TEXT2);
  g->setTextSize(1);
  g->setCursor(10, barY + 20);
  g->print(sub);
}

void drawDevicePage() {
  g->setTextColor(COL_TEXT);
  g->setTextSize(2);
  g->setCursor(10, 6);
  g->print("Device Stats");

  int cpuInt = (int)(cpuPercentAvg + 0.5f);
  uint16_t cpuColor = (cpuInt >= 80) ? COL_WARN : COL_BLUE;
  drawFullStatBlock(34, "CPU USAGE (loop duty cycle, live)", cpuInt, "", cpuColor);

  uint32_t flashUsed, flashTotal;
  int flashPct = flashPercent(flashUsed, flashTotal);
  uint16_t flashColor = (flashPct >= 80) ? COL_WARN : COL_BLUE;
  drawFullStatBlock(80, "FLASH USAGE", flashPct,
                     fmtKB(flashUsed) + " / " + fmtKB(flashTotal), flashColor);

  uint32_t ramUsed;
  int ramPct = staticRamPercent(ramUsed);
  uint16_t ramColor = (ramPct >= 80) ? COL_WARN : COL_BLUE;
  drawFullStatBlock(126, "STATIC RAM USAGE", ramPct,
                     fmtKB(ramUsed) + " / " + fmtKB(TOTAL_RAM_BYTES), ramColor);

  uint64_t sdUsed = 0, sdTotal = 0;
  int sdPct = sdCapacityPercent(sdUsed, sdTotal);
  uint16_t sdColor = (sdPct >= 80) ? COL_WARN : COL_BLUE;
  drawFullStatBlock(172, "SD CARD USAGE", sdPct,
                     sdPct >= 0 ? fmtGB(sdUsed) + " / " + fmtGB(sdTotal) : "SD CARD NOT FOUND",
                     sdColor);
}

// "OFFLINE" banner overlaid on top of whatever page 6 is already showing (a
// playing cat GIF, or the no-cats placeholder) — a solid bar behind the text
// keeps it legible over a busy GIF frame. Drawn last, right before presentFrame().
void drawOfflineBanner() {
  g->fillRect(0, 0, 304, 44, COL_BG);
  g->setTextColor(COL_TEXT);
  g->setTextSize(5);
  g->setCursor(47, 6);  // centered: "OFFLINE" is 7 chars * 30px = 210px; (304-210)/2 = 47
  g->print("OFFLINE");
}

// ── PAGE 6 RENDERING ───────────────────────────────────────
// AnimatedGIF file callbacks — the SD reads inside these run under sdMutex,
// which gifTick()/openRandomCat() hold around every gif.open/playFrame/close.
void* GIFOpenFile(const char* fname, int32_t* pSize) {
  gifFile = SD.open(fname);
  if (!gifFile) return nullptr;
  *pSize = gifFile.size();
  return (void*)&gifFile;
}
void GIFCloseFile(void* pHandle) {
  File* f = static_cast<File*>(pHandle);
  if (f) f->close();
}
int32_t GIFReadFile(GIFFILE* pFile, uint8_t* pBuf, int32_t iLen) {
  File* f = static_cast<File*>(pFile->fHandle);
  int32_t want = iLen;
  // Reading to the very last byte breaks a later seek() on this SD lib, so leave one.
  if ((pFile->iSize - pFile->iPos) < iLen) want = pFile->iSize - pFile->iPos - 1;
  if (want <= 0) return 0;
  int32_t got = (int32_t)f->read(pBuf, want);
  pFile->iPos = f->position();
  return got;
}
int32_t GIFSeekFile(GIFFILE* pFile, int32_t iPosition) {
  File* f = static_cast<File*>(pFile->fHandle);
  f->seek(iPosition);
  pFile->iPos = (int32_t)f->position();
  return pFile->iPos;
}

// One image line into the frame sprite (via `g`), honoring transparency and the
// disposal method — the canonical AnimatedGIF LovyanGFX draw path. Centered with
// the global gifXOffset/gifYOffset. presentFrame() is called by gifTick() once
// the whole frame's lines are in.
void GIFDraw(GIFDRAW* pDraw) {
  uint16_t usTemp[320];
  uint16_t* usPalette = pDraw->pPalette;
  int iWidth = pDraw->iWidth;
  if (iWidth > 320) iWidth = 320;
  int y = gifYOffset + pDraw->iY + pDraw->y;
  
  bool offline = !STATE.haveData;
  bool mixedMode = (currentPage == 6 && !offline);
  int max_y = mixedMode ? 220 : 240;
  if (y < 0 || y >= max_y) return;

  uint8_t* s = pDraw->pPixels;

  if (pDraw->ucDisposalMethod == 2) {  // restore-to-background: paint transparent as bg
    for (int x = 0; x < iWidth; x++)
      if (s[x] == pDraw->ucTransparent) s[x] = pDraw->ucBackground;
    pDraw->ucHasTransparency = 0;
  }

  if (pDraw->ucHasTransparency) {
    uint8_t ucTransparent = pDraw->ucTransparent;
    uint8_t* pEnd = s + iWidth;
    int x = 0;
    while (x < iWidth) {
      uint16_t* d = usTemp;
      int iCount = 0;
      uint8_t c = ucTransparent - 1;
      while (c != ucTransparent && s < pEnd) {   // run of opaque pixels
        c = *s++;
        if (c == ucTransparent) { s--; }
        else { *d++ = usPalette[c]; iCount++; }
      }
      if (iCount) {
        int startX = gifXOffset + pDraw->iX + x;
        int endX = startX + iCount;
        int clipStart = startX;
        int clipEnd = endX;
        if (mixedMode) {
          if (clipStart < 160) clipStart = 160;
          if (clipEnd > 304) clipEnd = 304;
        } else {
          if (clipStart < 0) clipStart = 0;
          if (clipEnd > 304) clipEnd = 304;
        }
        if (clipStart < clipEnd) {
          g->pushImage(clipStart, y, clipEnd - clipStart, 1, usTemp + (clipStart - startX));
          if (mixedMode) {
            if (y < gifMinY) gifMinY = y;
            if (y > gifMaxY) gifMaxY = y;
          }
        }
        x += iCount;
        iCount = 0;
      }
      c = ucTransparent;
      while (c == ucTransparent && s < pEnd) {   // run of transparent pixels to skip
        c = *s++;
        if (c == ucTransparent) iCount++;
        else s--;
      }
      x += iCount;
    }
  } else {
    for (int x = 0; x < iWidth; x++) usTemp[x] = usPalette[*s++];
    int startX = gifXOffset + pDraw->iX;
    int endX = startX + iWidth;
    int clipStart = startX;
    int clipEnd = endX;
    if (mixedMode) {
      if (clipStart < 160) clipStart = 160;
      if (clipEnd > 304) clipEnd = 304;
    } else {
      if (clipStart < 0) clipStart = 0;
      if (clipEnd > 304) clipEnd = 304;
    }
    if (clipStart < clipEnd) {
      g->pushImage(clipStart, y, clipEnd - clipStart, 1, usTemp + (clipStart - startX));
      if (mixedMode) {
        if (y < gifMinY) gifMinY = y;
        if (y > gifMaxY) gifMaxY = y;
      }
    }
  }
}

// Scan /cats/ for *.gif once at boot into catFiles[]. Names are normalized to a
// full "/cats/<name>" path (openNextFile()'s name() is basename-only here).
void scanCats() {
  catCount = 0;
  if (!STATE.sdOk) return;
  lockSD();
  File dir = SD.open(CATS_DIR);
  if (dir && dir.isDirectory()) {
    for (File f = dir.openNextFile(); f && catCount < MAX_CATS; f = dir.openNextFile()) {
      if (!f.isDirectory()) {
        String name = f.name();
        int slash = name.lastIndexOf('/');
        if (slash >= 0) name = name.substring(slash + 1);
        String lower = name; lower.toLowerCase();
        if (lower.endsWith(".gif")) catFiles[catCount++] = String(CATS_DIR) + "/" + name;
      }
      f.close();
    }
  }
  if (dir) dir.close();
  unlockSD();
  Serial.printf("[cats] %d GIF(s) in %s\n", catCount, CATS_DIR);
}

// "26% reset: 02:09" pinned to the bottom-left corner of the full-screen
// cat page (GIF_PAGE only), on a solid black box so it stays legible over
// any GIF frame. gifTick() runs unlocked on core 1, so STATE is copied under
// the lock before drawing.
void drawSessionResetOverlay() {
  lockState();
  int percent = STATE.sessionPercent;
  String resets = STATE.sessionResets;
  unlockState();
  if (percent < 0 || !resets.length()) return;

  String text = String(percent) + "% reset: " + resets;
  int padX = 4, padY = 4;
  int textW = (int)text.length() * 12;  // textSize 2 = 12px/char wide, 16px tall
  int boxW = textW + padX * 2, boxH = 16 + padY * 2;
  int boxY = 240 - boxH;
  g->fillRect(0, boxY, boxW, boxH, 0x0000);
  g->setTextSize(2);
  g->setCursor(padX, boxY + padY);
  g->setTextColor(COL_TEXT);
  g->print(String(percent) + "% ");
  g->setTextColor(COL_TEXT2);
  g->print("reset: ");
  g->setTextColor(COL_TEXT);
  g->print(resets);
}

// A centered message when there are no cats to show (no SD, or empty /cats/).
// Drawn once per page visit (gifPlaceholderDrawn) so it doesn't flicker; the
// OFFLINE banner is re-applied on top every call since offline state can
// change independently of the placeholder's draw-once guard.
void drawGifPlaceholder(bool offline) {
  bool mixedMode = (currentPage == 6 && !offline);
  if (!gifPlaceholderDrawn) {
    gifPlaceholderDrawn = true;
    if (mixedMode) {
      g->fillRect(160, 0, 160, 240, COL_BG);
      g->setTextColor(COL_ACCENT);
      g->setTextSize(2);
      g->setCursor(202, 92);
      g->print("CATS");
      g->setTextColor(COL_TEXT2);
      g->setTextSize(1);
      const char* msg = "no GIFs";
      g->setCursor(160 + (144 - (int)strlen(msg) * 6) / 2, 120);
      g->print(msg);
    } else {
      g->fillScreen(COL_BG);
      g->setTextColor(COL_ACCENT);
      g->setTextSize(3);
      g->setCursor(116, 92);  // "CATS" = 4 chars * 18px = 72; (304-72)/2 = 116
      g->print("CATS");
      g->setTextColor(COL_TEXT2);
      g->setTextSize(1);
      const char* msg = STATE.sdOk ? "no GIFs found in /cats/ on the SD card"
                                   : "insert an SD card with /cats/ GIFs";
      g->setCursor((304 - (int)strlen(msg) * 6) / 2, 132);
      g->print(msg);
    }
  }
  if (currentPage == GIF_PAGE) drawSessionResetOverlay();
  if (offline) drawOfflineBanner();
  presentFrame();
}

// Open a random cat, clearing the sprite first so a smaller GIF letterboxes on
// black rather than over the previous cat. Returns false if the file won't open.
bool openRandomCat() {
  if (catCount == 0 || !gif) return false;
  gif->begin(GIF_PALETTE_RGB565_BE);  // this panel's pushImage path wants big-endian RGB565 (LE showed swapped colors)
  int i = (int)random(catCount);
  lockSD();
  int ok = gif->open(catFiles[i].c_str(), GIFOpenFile, GIFCloseFile,
                     GIFReadFile, GIFSeekFile, GIFDraw);
  unlockSD();
  if (!ok) return false;
  int w = gif->getCanvasWidth(), h = gif->getCanvasHeight();
  bool offline = !STATE.haveData;
  if (currentPage == 6 && !offline) {
    gifXOffset = 160 + (144 - w) / 2;
    gifYOffset = (220 - h) / 2;
    g->fillRect(160, 0, 160, 240, 0x0000); // clear only the right side
  } else {
    gifXOffset = w < 304 ? (304 - w) / 2 : 0;
    gifYOffset = h < 240 ? (240 - h) / 2 : 0;
    g->fillScreen(0x0000);  // clear the sprite; first frame's lines land on top, then present
  }
  gifOpen = true;
  gifFirstFrame = true;
  return true;
}

// Called from loop() on core 1 while page 6 is showing, OR while offline on any
// page (cats double as the offline screen — see catMode in loop()). Decodes at
// most one frame per call (pacing itself via gifNextFrameMs) so touch stays
// responsive; when a GIF ends it immediately opens another at random — endless
// cats. `offline` overlays the OFFLINE banner on top of the cat frame.
void gifTick(bool offline) {
  if (!STATE.sdOk || catCount == 0 || !gif) { drawGifPlaceholder(offline); return; }
  uint32_t now = millis();
  if (gifOpen && now < gifNextFrameMs) return;  // not time for the next frame yet
  if (!gifOpen && !openRandomCat()) {
    drawGifPlaceholder(offline);
    gifNextFrameMs = now + 1000;  // retry opening later
    return;
  }
  int delayMs = 0;
  if (gifFirstFrame) {
    gifMinY = 0;
    gifMaxY = 219;
  } else {
    gifMinY = 220;
    gifMaxY = -1;
  }
  lockSD();
  int more = gif->playFrame(false, &delayMs);  // bSync=false: we handle timing ourselves
  unlockSD();
  if (gifFirstFrame) {
    gifFirstFrame = false;
  }
  if (currentPage == GIF_PAGE) drawSessionResetOverlay();
  if (offline) drawOfflineBanner();
  if (mixedPageDirty) {
    presentFrame(true);  // flush the static card update
    mixedPageDirty = false;
  } else {
    presentFrame(false); // push only the GIF region
  }
  if (more == 0) {                 // last frame drawn — rotate to a new random cat
    lockSD(); gif->close(); unlockSD();
    gifOpen = false;
    gifNextFrameMs = now;          // open the next one on the following tick
  } else {
    gifNextFrameMs = now + (delayMs > 0 ? delayMs : 80);
  }
}

void render() {
  // Page 6 is animated frame-by-frame by gifTick() in loop(), not drawn here.
  // Offline mode is also driven by gifTick() (cats + an OFFLINE banner, see
  // catMode in loop()), so render() is never called while offline — no
  // separate offline branch needed here.
  if (currentPage == GIF_PAGE || currentPage == 6) return;
  // Hold the lock across all STATE reads (draw helpers read String members the
  // network task may reassign), then release before the SPI pushSprite so the
  // task's next brief STATE copy isn't delayed by the display write.
  lockState();
  g->fillScreen(COL_BG);
  switch (currentPage) {
    case 0: drawStatusPage(); break;
    case 1: drawProjectsPage(); break;  // projects (7d) + 7-day trend combined
    case 2: drawHomePage(); break;
    case 3: drawDevicePage(); break;
    case 4: drawLongTrendPage(); break;
  }
  drawFooter();
  unlockState();
  presentFrame();
}

// ── NETWORK TASK ───────────────────────────────────────────
// All blocking I/O (usage poll — which now also carries BTC/weather — mDNS,
// WiFi reconnect, SD writes) runs here on core 0, so loop() on core 1 keeps
// sampling touch and repainting even while a fetch stalls. Only the brief STATE
// copies inside the fetch helpers take stateMutex; the network waits hold none.
void networkTask(void* param) {
  uint32_t lastUsagePollMs = 0;
  uint32_t lastHeapLogMs = 0;
  bool lowHeapLogged = false;
  for (;;) {
    uint32_t now = millis();

    // Black-box heartbeat: append free-heap + uptime to /diag_log.csv hourly,
    // plus a one-shot warning the first time free heap dips below 20KB — the
    // paper trail for diagnosing a fragmentation-driven reboot after the fact.
    if (now - lastHeapLogMs >= 3600000UL) {
      lastHeapLogMs = now;
      logDiag((String("heap free=") + ESP.getFreeHeap() +
               " uptime_min=" + (now / 60000)).c_str());
    }
    if (ESP.getFreeHeap() < 20000 && !lowHeapLogged) {
      lowHeapLogged = true;
      logDiag((String("low_heap free=") + ESP.getFreeHeap()).c_str());
    }

    if (now - lastUsagePollMs >= POLL_INTERVAL_MS) {
      lastUsagePollMs = now;
      lastPollMs = now;  // drives the footer progress line on the render side
      if (WiFi.status() == WL_CONNECTED) {
        if (wifiDownCycles > 0) {
          logDiag(("wifi_recovered after " + String(wifiDownCycles) + " cycles").c_str());
        }
        wifiDownCycles = 0;
        ensureMdns();
        lockState();
        int64_t prevTodayTokens = STATE.trend[6];
        unlockState();
        connected = fetchUsage();
        struct tm nowInfo;
        if (connected && getLocalTime(&nowInfo, 0)) {
          appendDailyLogIfNeeded(nowInfo, prevTodayTokens);
        }
      } else {
        if (wifiDownCycles == 0) logDiag("wifi_down");
        connected = false;
        mdnsStarted = false;       // re-init mDNS once WiFi returns
        serverIpResolved = false;  // re-resolve the Mac's (possibly new) IP
        wifiDownCycles++;
        WiFi.reconnect();
        if (wifiDownCycles >= RESTART_AFTER_CYCLES) {
          logDiag("restart_wifi_timeout");
          ESP.restart();
        }
      }

      // A failed poll here covers both WiFi being down and WiFi being up but
      // the Mac unreachable. haveData is otherwise sticky-true, so this is the
      // only thing that ever flips the display back to the offline/cat screen.
      if (connected) {
        STATE.haveData = true;
        fetchFailCycles = 0;
      } else {
        if (!STATE.haveData && STATE.sdOk) STATE.haveData = loadCachedUsage();
        if (fetchFailCycles < OFFLINE_AFTER_CYCLES) fetchFailCycles++;
        if (fetchFailCycles >= OFFLINE_AFTER_CYCLES) STATE.haveData = false;
      }
    }

    // BTC + weather arrive inside the /api/usage payload (fetched by the Mac),
    // so there are no separate market fetches on the board anymore.

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// ── SETUP / LOOP ───────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  gfx.init();
  gfx.setRotation(1);  // landscape 320x240; use 3 if the image is upside down
  gfx.setBrightness(200);  // provisional; re-applied from /config.json once SD is up

  // Create before any fetch: applyUsageJson locks it even
  // during setup's single-threaded initial fetch (uncontended there).
  stateMutex = xSemaphoreCreateMutex();
  sdMutex = xSemaphoreCreateMutex();  // must exist before the first logDiag/SD access below
  randomSeed(esp_random());           // so the cat picked on page 6 differs each boot

  // Allocate the frame buffer before WiFi grabs heap. 16-bit needs ~154KB
  // contiguous; if that fails, 8-bit (~77KB, slight color quantization).
  frame.setColorDepth(16);
  if (!frame.createSprite(320, 240)) {
    frame.setColorDepth(8);
    if (!frame.createSprite(320, 240)) {
      g = &gfx;  // last resort: draw directly on the panel
      Serial.println("[frame] sprite alloc FAILED, drawing direct (will flicker)");
    } else {
      g = &frame;
      Serial.println("[frame] using 8-bit frame buffer");
    }
  } else {
    g = &frame;
    Serial.println("[frame] using 16-bit frame buffer");
  }

  // Mount the SD card on its own dedicated HSPI bus (see pins.h/sdSPI) —
  // independent of WiFi/network state, and only attempted once: SD I/O is
  // blocking, so a flaky card must not stall the 20s poll loop.
  sdSPI.begin(CYD_SD_SCLK, CYD_SD_MISO, CYD_SD_MOSI, CYD_SD_CS);
  STATE.sdOk = SD.begin(CYD_SD_CS, sdSPI, 20000000); // 20 MHz SPI
  if (STATE.sdOk) {
    loadRuntimeConfig();
    gfx.setBrightness(cfgBrightness);  // honor the /config.json override
    loadLongTrendFromSD();
    loadEnvCache();     // show last-known BTC/weather immediately, before any live fetch
    showBootSplash();   // optional /splash.bmp, briefly, before the WiFi spinner
    scanCats();         // index /cats/*.gif for the page-6 player
  } else {
    Serial.println("[sd] card not found or failed to mount");
  }
  gfx.applyRuntimeConfig(cfgScreenRotation, cfgTouchXMin, cfgTouchXMax, cfgTouchYMin, cfgTouchYMax, cfgTouchOffsetRotation);
  logDiag((String("boot reason=") + resetReasonStr()).c_str());

  connectWifi();
  connected = fetchUsage();  // also carries BTC + weather from the Mac
  STATE.haveData = connected;
  if (!connected && STATE.sdOk) STATE.haveData = loadCachedUsage();
  render();
  lastPollMs = millis();

  uint32_t flashUsed, flashTotal, ramUsed;
  Serial.printf("[device] flash=%d%% (%lu/%lu B) static_ram=%d%% (%lu/%lu B) free_heap=%lu B\n",
                flashPercent(flashUsed, flashTotal), (unsigned long)flashUsed, (unsigned long)flashTotal,
                staticRamPercent(ramUsed), (unsigned long)ramUsed, (unsigned long)TOTAL_RAM_BYTES,
                (unsigned long)ESP.getFreeHeap());

  // Hand all blocking network I/O to core 0; loop() below stays on core 1 and
  // never blocks on a fetch. 8192-byte stack matches the Arduino loopTask that
  // previously ran these same TLS calls; priority 1 = same as loopTask.
  xTaskCreatePinnedToCore(networkTask, "net", 8192, nullptr, 1, nullptr, 0);
}

void loop() {
  uint32_t loopStartUs = micros();
  uint32_t now = millis();

  // Cats own the screen on page 6, AND whenever offline (the cat GIF loop
  // doubles as the offline screen, with an OFFLINE banner overlaid on top —
  // see drawOfflineBanner()/gifTick()).
  bool offline = !STATE.haveData;
  bool catMode = (currentPage == GIF_PAGE) || (currentPage == 6) || offline;

  // catMode-transition bookkeeping: close the GIF on the way out, and reset
  // the placeholder/timer on the way in.
  static bool prevCatMode = false;
  if (catMode != prevCatMode) {
    if (!catMode) {
      // Leaving cat mode: close any open GIF and free the decoder's ~24KB so the
      // BTC/weather HTTPS fetches on core 0 have heap for their TLS handshakes.
      if (gif && gifOpen) { lockSD(); gif->close(); unlockSD(); }
      gifOpen = false;
      delete gif;
      gif = nullptr;
    } else {
      if (!gif) gif = new AnimatedGIF();  // allocate the decoder only while it's needed
      gifPlaceholderDrawn = false;
      gifNextFrameMs = 0;
    }
    prevCatMode = catMode;
  }

  if (catMode) {
    // Cats animate frame-by-frame; they own the whole screen (no footer or
    // poll-progress line) and need no 1s repaint.
    gifTick(offline);
    if (currentPage == 6 && !offline) {
      static uint32_t lastMixedRenderMs = 0;
      if (now - lastMixedRenderMs >= 1000) {
        lastMixedRenderMs = now;
        lockState();
        drawMixedPageStatic();
        unlockState();
        mixedPageDirty = true;
      }
    }
  } else {
    // Repaint once a second for the pulsing status dot, the local session
    // countdown, and the Bangkok clock's ticking seconds. Fetches run on core 0,
    // so this cadence is unaffected by network state.
    static uint32_t lastRenderMs = 0;
    if (now - lastRenderMs >= 1000) {
      lastRenderMs = now;
      render();
    }

    // Smooth the bottom progress line between repaints: draw only the newly
    // elapsed pixels straight to the panel (~every 30ms loop pass) instead of
    // pushing a full frame, which at 1s intervals looked stuttery.
    static int lineW = 0;
    if (connected) {
      uint32_t elapsed = now - lastPollMs;
      if (elapsed > POLL_INTERVAL_MS) elapsed = POLL_INTERVAL_MS;
      int w = (int)((float)elapsed / POLL_INTERVAL_MS * 320);
      if (w < lineW) {
        lineW = w;  // new poll cycle: the full repaint already cleared the line
      } else if (w > lineW) {
        gfx.fillRect(lineW, 239, w - lineW, 1, COL_TEXT2);
        lineW = w;
      }
    } else {
      lineW = 0;
    }
  }

  int32_t tx, ty;
  bool touchDown = gfx.getTouch(&tx, &ty);
  if (touchDown != touchWasDown) {
    Serial.printf("[touch] down=%d x=%d y=%d\n", touchDown, tx, ty);
  }
  if (touchDown && !touchWasDown && now - lastTouchMs > TOUCH_DEBOUNCE_MS) {
    lastTouchMs = now;
    bool isRight = (tx >= 160);
    if (isRight) {
      currentPage = (currentPage + 1) % PAGE_COUNT;
    } else {
      currentPage = (currentPage - 1 + PAGE_COUNT) % PAGE_COUNT;
    }
    
    // Handle initialization when entering a page
    if (currentPage == 6 && !offline) {
      lockState();
      g->fillScreen(COL_BG);
      drawMixedPageStatic();
      unlockState();
      presentFrame();
      
      if (gifOpen) { lockSD(); gif->close(); unlockSD(); }
      gifOpen = false;
      gifNextFrameMs = 0;
    } else if (currentPage == GIF_PAGE) {
      if (gifOpen) { lockSD(); gif->close(); unlockSD(); }
      gifOpen = false;
      gifNextFrameMs = 0;
    }
    
    if (!catMode) render();  // catMode: gifTick() already redraws every pass
    flashTouchBorder(isRight);  // one-frame white edge flash on the new page: touch registered
  }
  touchWasDown = touchDown;

  // Duty-cycle CPU estimate: work time this iteration vs. the fixed 30ms
  // delay below, smoothed with an EMA (see cpuPercentAvg declaration).
  uint32_t busyUs = micros() - loopStartUs;
  float sample = (float)busyUs / (busyUs + 30000.0f) * 100.0f;
  cpuPercentAvg = cpuPercentAvg * 0.9f + sample * 0.1f;

  delay(30);
}
