// Shared declarations for the CYD dashboard firmware, split (2026) out of what
// was previously one 2920-line cyd_dashboard.ino. Every .cpp file in this
// sketch includes this header; it is the only place cross-file globals,
// mutexes, and function prototypes are declared. Definitions live in exactly
// one .cpp each (see the comment above each block below for where).
//
// Arduino's auto-prototype generation only applies within the primary .ino —
// every other .cpp here is an ordinary translation unit that sees only what
// it #includes, so this header is load-bearing: skipping a declaration here
// causes a link error in a different file, not a compile error in this one.
#pragma once

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
#include <DNSServer.h>
#include <WebServer.h>

#include "pins.h"
#include "config.h"

// ── DISPLAY ────────────────────────────────────────────────
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

// Defined (in this declaration order, same TU) in cyd_dashboard.ino, so their
// constructors run in a well-defined sequence — frame's ctor just stores
// &gfx, it doesn't need gfx fully constructed yet, but keeping all four in
// one TU sidesteps any static-init-order question entirely.
extern LGFX gfx;
extern SPIClass sdSPI;
extern LGFX_Sprite frame;
extern lgfx::LovyanGFX* g;

void presentFrame(bool fullScreen = true);
void flashTouchBorder(bool isRight);
bool pixelShiftTick(uint32_t now);
void clearShiftMargins();
bool checkHourlyFlash(bool& isEvenSecond);
void shineTick(uint32_t nowMs);

// Non-const: overridable from /config.json (see sd_store.cpp's
// loadRuntimeConfig). Originally set once at boot before the two tasks
// start; also settable at runtime from the Settings page's Poll Interval
// leaf (loop(), core 1), so it's volatile -- networkTask (core 0) reads it
// every cycle. Defined in cyd_dashboard.ino.
extern volatile uint32_t POLL_INTERVAL_MS;

// ── SHARED CONSTANTS ───────────────────────────────────────
// Internal linkage per TU (C++ global `const` default) — safe to define
// identically in every file that includes this header; no ODR issue.
const uint32_t TOUCH_DEBOUNCE_MS = 350;
const int PAGE_COUNT = 9;
const int GIF_PAGE = 6;    // 7th page (0-indexed): random cat GIFs from /cats/ on SD
const int MIXED_PAGE = 7;  // 8th page: status + cats split
const int BTC_TICKER_PAGE = 8; // 9th page: Binance BTC candlestick ticker

const int DOT_SPACING = 12;
const int DOT_START_X = 300 - (PAGE_COUNT - 1) * DOT_SPACING;

const int PULSE_HIT_X0 = 0, PULSE_HIT_X1 = 40, PULSE_HIT_Y0 = 214, PULSE_HIT_Y1 = 240;

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
const uint16_t COL_SHINE_LO = 0x5ECE;  // COL_GOOD lerped ~25% to white (shine band edge)
const uint16_t COL_SHINE_MID = 0x9734; // ~50% to white (shine band mid)
const uint16_t COL_SHINE_HI = 0xD7BA;  // ~80% to white (shine band center)
const uint16_t COL_WARN = 0xF8C6;    // rose
const uint16_t COL_TRACK = 0x5ACB;   // neutral grey bar track
const uint16_t COL_TRACK_BLACK = 0x0000; // pure-black bar track (reset-countdown bars)
const uint16_t COL_BLUE = 0x3C1E;    // device-stats bar chart accent
const uint16_t COL_YELLOW = 0xFFE0;  // yellow for sun/lightning icons
// Weather page background — pure black (same as other pages' near-black COL_BG
// family). Used only by drawWeatherPage.
const uint16_t COL_WEATHER_BG = 0x0000;
// Weather card hit-box on the status page (page 0): fillRoundRect(150,170,60,46).
// Tap opens the Weather overlay (mirrors settings' PULSE_HIT_* pattern).
const int WEATHER_HIT_X0 = 150, WEATHER_HIT_X1 = 210;
const int WEATHER_HIT_Y0 = 170, WEATHER_HIT_Y1 = 216;

// Weather forecast slots delivered by /api/usage (Mac-proxied Open-Meteo /
// WeatherAPI) and cached on SD as /weather.json. Fixed-size arrays — no
// String/heap churn on every poll.
const int WEATHER_HOURLY_N = 6;
const int WEATHER_DAILY_N = 5;
struct WeatherHour {
  int8_t hour = -1;   // 0-23 local; -1 = empty slot
  int8_t tempC = 0;
  int16_t code = -1;  // WMO weather_code
};
struct WeatherDay {
  int8_t wday = -1;   // 0=Sun .. 6=Sat (tm_wday); -1 = empty
  int8_t high = 0;
  int8_t low = 0;
  int16_t code = -1;
};


// ── STATE ──────────────────────────────────────────────────
// projectNames/sessionResets/weekResets/weekModelName/weekModelResets are
// fixed char buffers rather than String: these fields get reassigned every
// ~20s poll for weeks of uptime on a no-PSRAM board, and repeated String
// reassignment (realloc when the new value's length differs from the old
// capacity) is exactly the kind of long-run heap churn that eventually
// fragments a small heap. Local, short-lived String concatenation elsewhere
// in the draw code (building one line of text, then discarding it before the
// next statement) doesn't have this problem and is left alone.
struct CandleRec {
  uint32_t openEpoch;
  float o, h, l, c;
};

const int CANDLE_COUNT = 288;

struct UsageState {
  char projectNames[5][32];
  int64_t projectTokens[5];
  int projectCount = 0;
  int64_t trend[7];
  int64_t last24hTokens = 0;  // rolling trailing-24h total, for the 30-Day Trend page's
                              // "today" bar — trend[6] is midnight-to-now and only equals
                              // a full day right before the day rolls over
  int sessionPercent = -1;   // -1 = limits unavailable
  char sessionResets[24] = "";
  long sessionResetsInSec = -1;  // countdown to session reset; -1 = unknown
  int weekPercent = -1;
  char weekResets[24] = "";
  long weekResetsInSec = -1;     // countdown to week reset; -1 = unknown
  long ctxTokens = -1;       // context window of the latest session; -1 = unknown
  int ctxPercent = -1;
  int weekModelPercent = -1; // per-model weekly limit; -1 = absent (row hidden)
  char weekModelName[24] = "";      // e.g. "Fable"
  char weekModelResets[24] = "";
  float creditsUsed = -1;    // extra-usage dollars; -1 = unavailable
  float creditsLimit = -1;
  int creditsPercent = -1;
  uint32_t lastFetchOkMs = 0;
  double btcPrice = -1;      // BTC/USDT (from the Mac via /api/usage); -1 = unknown
  float btcChangePct = NAN;
  CandleRec btcCandles[288];
  int btcCandleCount = 0;
  float weatherTempC = -999; // Bangkok temp (from the Mac via /api/usage); -999 = unknown
  int weatherCode = -1;      // WMO weather_code (from the Mac); -1 = unknown
  // Weather page fields — same payload as the status card's temp/code, plus
  // today's H/L, a short condition label, place name, next 6h, and next 5d.
  // -999 / -1 / empty mean "not received yet" (show "--" / hide row).
  int weatherHigh = -999;
  int weatherLow = -999;
  char weatherCondition[20] = "";
  char weatherPlace[16] = "Bangkok";
  WeatherHour weatherHourly[WEATHER_HOURLY_N];
  uint8_t weatherHourlyCount = 0;
  WeatherDay weatherDaily[WEATHER_DAILY_N];
  uint8_t weatherDailyCount = 0;
  bool sdOk = false;
  int lastLoggedYday = -1;  // tm_yday of the last day appended to the SD log
  // Written by networkTask() (core 0) without stateMutex (see state.h's lock
  // comment: only the brief result-copy takes the lock, not this flag), read
  // by loop()/presentFrame()/GIFDraw()/openCatAtIndex() on core 1 -- volatile
  // for the same cross-core-visibility reason POLL_INTERVAL_MS is, since nothing
  // here serializes it.
  volatile bool haveData = false;    // true once any data (live or SD-cached) has been applied
};
// Defined in cyd_dashboard.ino.
extern UsageState STATE;

// 30-day on-device history, backed by /daily_log.csv on the SD card. Defined
// in cyd_dashboard.ino; read/written from sd_store.cpp and pages.cpp.
const int LONG_TREND_DAYS = 30;
extern int64_t longTrend[LONG_TREND_DAYS];
extern int longTrendCount;

// Page/navigation state — owned by loop()'s touch handler (cyd_dashboard.ino),
// read by pages.cpp/settings.cpp/gif_player.cpp to know what's on screen.
extern int currentPage;
extern int cfgBootPage;
// Weather detail overlay (opened by tapping the weather card on page 0).
// Not a swipe-cycle page — same pattern as settingsScreen: any tap exits.
extern bool weatherPageOpen;
enum SettingsScreen { SET_OFF, SET_LIST, SET_LEAF };
extern SettingsScreen settingsScreen;
extern int settingsScrollOffset;  // vertical scroll position (px) of the SET_LIST list
extern int settingsLeafIndex;
const uint32_t CONFIRM_ARM_MS = 4000;
extern uint32_t confirmArmedMs;
extern int confirmArmedRow;
extern bool mixedPageDirty;
// Written by networkTask() (core 0), read by drawFooter()/loop()'s progress
// line (core 1) with no lock -- volatile for the same reason as
// STATE.haveData above.
extern volatile uint32_t lastPollMs;
extern uint32_t lastTouchMs;
// Dirty-band bounds for the mixed page's partial GIF-region push; set by
// gif_player.cpp's gifTick()/GIFDraw(), read by presentFrame() in pages.cpp.
extern int gifMinY;
extern int gifMaxY;

// ── PIXEL SHIFT (anti image-retention) ─────────────────────
const int8_t SHIFT_ORBIT[8][2] = {
  {0, 0}, {1, 0}, {2, 0}, {2, -1}, {2, -2}, {1, -2}, {0, -2}, {0, -1}
};
extern uint32_t cfgShiftStepMs;  // dwell per step; /config.json "pixel_shift_min", 0 disables
extern uint8_t shiftIdx;
extern int shiftX, shiftY;
extern uint32_t lastShiftMs;
extern bool shiftDirty;

// CPU load estimate, WiFi/connection status.
extern float cpuPercentAvg;
extern bool touchWasDown;
// Set in net.cpp's connectWifi()/fetchUsage() (core 0 once networkTask is
// running); audited for cross-core reads and found to have none — nothing
// else in the firmware currently reads wifiOk, so it doesn't need volatile.
// Left as a plain bool rather than force-adding the keyword without a reason.
extern bool wifiOk;
// Did the most recent fetch reach the server? Written by networkTask() (core
// 0), read by drawFooter()/loop()'s progress line (core 1) with no lock --
// volatile for the same cross-core-visibility reason as STATE.haveData.
extern volatile bool connected;

// Guards every read/write of the shared UsageState (and longTrend[]) between
// the render loop (core 1) and the network task (core 0). Non-recursive —
// draw helpers must never re-lock. See cyd_dashboard.ino's networkTask()/
// pages.cpp's render() for the two sides of this.
extern SemaphoreHandle_t stateMutex;
inline void lockState()   { if (stateMutex) xSemaphoreTake(stateMutex, portMAX_DELAY); }
inline void unlockState() { if (stateMutex) xSemaphoreGive(stateMutex); }

// Serializes the SD/HSPI bus between the two cores (networkTask on core 0,
// and the page-6 GIF player reading frames from SD on the render core, core
// 1). NON-RECURSIVE and NON-NESTABLE with stateMutex in the reverse order:
// the only nesting allowed is sdMutex -> stateMutex.
extern SemaphoreHandle_t sdMutex;
inline void lockSD()   { if (sdMutex) xSemaphoreTake(sdMutex, portMAX_DELAY); }
inline void unlockSD() { if (sdMutex) xSemaphoreGive(sdMutex); }

// Cat-shuffle interval: how long each cat GIF plays before rotating to a new
// random one. Defined in gif_player.cpp (gifTick reads it); settings.cpp's
// Cat Shuffle setting reads/writes it via getCurrentCatShuffle/applyCatShuffle.
extern uint32_t catShuffleMs;

// Runtime overrides for the compiled config.h defaults, loaded from an
// optional /config.json on the SD card (see sd_store.cpp's loadRuntimeConfig).
// Defined in cyd_dashboard.ino.
extern String cfgWifiSsid;
extern String cfgWifiPassword;
extern String cfgServerHost;
extern int cfgServerPort;
extern int cfgBrightness;
extern bool cfgNightModeOn;
extern bool nightDimActive;
const uint8_t NIGHT_MODE_DIM_VALUE = 64;  // ~25%, matches the brightness preset
// Green reset-countdown bars (under 5h/week) + analog-clock timer wedge.
// Green reset hand on the clock is always drawn when a reset is known.
extern bool cfgShowCountdown;
extern int cfgScreenRotation;
extern int cfgTouchXMin;
extern int cfgTouchXMax;
extern int cfgTouchYMin;
extern int cfgTouchYMax;
extern int cfgTouchOffsetRotation;

// Generic Settings-page persistence queue: a leaf's apply() (loop(), core 1)
// mutates its live global directly, then queues the /config.json key/value
// here; networkTask (core 0) drains it so the SD write never happens on the
// render core. Defined in cyd_dashboard.ino (networkTask drains it);
// queued from settings.cpp.
extern volatile bool pendingConfigSave;
extern volatile uint8_t pendingConfigKeyId;
extern volatile int32_t pendingConfigValue;
extern volatile bool pendingForgetWifi;

// Config keys persisted through the generic queue above (see
// sd_store.cpp's saveIntConfigToSD and settings.cpp's queueConfigSave).
enum ConfigKeyId {
  CFGKEY_BRIGHTNESS = 0, CFGKEY_POLL_INTERVAL, CFGKEY_PIXEL_SHIFT, CFGKEY_BOOT_PAGE,
  CFGKEY_CAT_SHUFFLE, CFGKEY_NIGHT_MODE, CFGKEY_ROTATION, CFGKEY_SHOW_COUNTDOWN, CFGKEY_COUNT
};
extern const char* const CONFIG_KEY_NAMES[CFGKEY_COUNT];

// ── FORMATTING (format.cpp) ────────────────────────────────
String fmtTokens(int64_t t);
String fmtCost(float c);
String fmtBtc(double p);
String fmtCountdown(long sec);
String fmtCountdownDHM(long sec);
String fmtKB(uint32_t bytes);
String fmtGB(uint64_t bytes);
int flashPercent(uint32_t &usedOut, uint32_t &totalOut);
int staticRamPercent(uint32_t &usedOut);
int sdCapacityPercent(uint64_t &usedOut, uint64_t &totalOut);

// ── NETWORK (net.cpp) ──────────────────────────────────────
void ensureMdns();
void connectWifi();
bool resolveServer();
bool fetchUsage();
bool loadCachedUsage();
void loadEnvCache();
void loadWeatherCache();
bool applyUsageJson(const String& payload);
// Set false by networkTask() (cyd_dashboard.ino) on WiFi loss, so ensureMdns()
// re-initializes once WiFi returns.
extern bool mdnsStarted;

// ── SD STORE (sd_store.cpp) ────────────────────────────────
void loadRuntimeConfig();
const char* resetReasonStr();
void logDiag(const char* event);
void loadLongTrendFromSD();
void appendDailyLogIfNeeded(const struct tm& nowInfo, int64_t justEndedDayTokens);
bool drawBmpFromSD(const char* path, int dx, int dy);
void showBootSplash();
void saveWifiCredsToSD(const String& ssid, const String& password);
void saveIntConfigToSD(const char* key, int32_t value);
void forgetWifiFromSD();

// ── PAGES / RENDER (pages.cpp) ─────────────────────────────
void render();
void drawOfflineBanner();
void drawMixedPageStatic();

// ── GIF PLAYER (gif_player.cpp) ─────────────────────────────
void scanCats();
void gifTick(bool offline);
// Encapsulate what used to be loop() reaching directly into gif/gifOpen/
// gifNextFrameMs — narrows the extern surface to three intent-named calls
// instead of four raw globals.
void gifPlayerEnterCatMode();       // allocate the decoder on entering a cat page (or offline)
void gifPlayerExitCatMode();        // free the decoder on leaving
void gifPlayerResetForPageChange(); // force a fresh random GIF on the next tick

// ── SETTINGS (settings.cpp) ────────────────────────────────
void renderSettings();
const extern int SETTINGS_COUNT;
void queueConfigSave(uint8_t keyId, int32_t value);
// Handles a tap while settingsScreen == SET_LEAF; returns true if it consumed
// the tap. Keeps the SettingDef/button-layout internals out of
// cyd_dashboard.ino entirely.
bool handleSettingsTouch(int32_t tx, int32_t ty, uint32_t now, bool catMode);
// SET_LIST is drag-to-scroll rather than tap-driven, so it needs the full
// down/move/up gesture instead of one tap callback: Begin records the touch
// start, Move live-updates settingsScrollOffset while the finger is down, End
// decides whether the gesture was a tap (opens a leaf / exits) or a scroll
// (total movement over DRAG_TAP_PX in settings.cpp) and does nothing further.
void settingsListDragBegin(int32_t tx, int32_t ty);
void settingsListDragMove(int32_t tx, int32_t ty);
void settingsListDragEnd(bool catMode);

// ── AP SETUP (ap_setup.cpp) ────────────────────────────────
void runApSetup();  // blocks until configured, then ESP.restart()s — never returns
