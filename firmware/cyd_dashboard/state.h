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
#include <Preferences.h>

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

// Non-const: overridable from flash (see sd_store.cpp's
// loadRuntimeConfig). Originally set once at boot before the two tasks
// start; also settable at runtime from the Settings page's Poll Interval
// leaf (loop(), core 1), so it's volatile -- networkTask (core 0) reads it
// every cycle. Defined in cyd_dashboard.ino.
//
// cfgPollIntervalSec is the user's chosen rate (what the Poll Interval leaf
// shows/saves). POLL_INTERVAL_MS is the *effective* interval networkTask
// uses — Battery Save may stretch it via applyEffectivePoll().
extern volatile uint32_t cfgPollIntervalSec;
extern volatile uint32_t POLL_INTERVAL_MS;

// ── SHARED CONSTANTS ───────────────────────────────────────
// Internal linkage per TU (C++ global `const` default) — safe to define
// identically in every file that includes this header; no ODR issue.
const uint32_t TOUCH_DEBOUNCE_MS = 350;
const int PAGE_COUNT = 5;
const int GIF_PAGE = 3;    // 4th page (0-indexed): random cat GIFs from /cats/ on SD
const int MIXED_PAGE = 4;  // 5th page: status + cats split

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
// Weather card hit-box on the status page (page 0): matches the full card
// drawn at fillRect(161,166,157,52) (pages.cpp). Tap opens the Weather
// overlay (mirrors settings' PULSE_HIT_* pattern).
const int WEATHER_HIT_X0 = 161, WEATHER_HIT_X1 = 318;
const int WEATHER_HIT_Y0 = 166, WEATHER_HIT_Y1 = 218;

// Footer CPU/ROM/RAM stats hit-box (drawFooter()'s "CPU x%  ROM x%  RAM x%"
// line, x0 picked to start right where the PULSE_HIT zone ends so the two
// don't compete). Tap opens the Device Stats overlay (mirrors weatherPageOpen's
// pattern) -- Device Stats is no longer one of the swiped PAGE_COUNT pages.
const int DEVICE_HIT_X0 = 40, DEVICE_HIT_X1 = 210;
const int DEVICE_HIT_Y0 = 214, DEVICE_HIT_Y1 = 240;

// Battery Save top-right corner overlay: y-range of drawBatterySaveIcon()'s
// backing box (pages.cpp), needed by gif_player.cpp to fold the icon's rows
// into the mixed page's dirty-band partial push (see gifTick()).
const int BATTERY_ICON_Y0 = 2, BATTERY_ICON_Y1 = 14;

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
struct UsageState {
  char projectNames[5][32];
  int64_t projectTokens[5];
  int projectCount = 0;
  int64_t trend[7];
  int sessionPercent = -1;   // -1 = limits unavailable
  char sessionResets[24] = "";
  long sessionResetsInSec = -1;  // countdown to session reset; -1 = unknown
  int weekPercent = -1;
  char weekResets[24] = "";
  long weekResetsInSec = -1;     // countdown to week reset; -1 = unknown
  int64_t ctxTokens = -1;    // context window of the latest session; -1 = unknown -- int64_t
                             // for consistency with the other token fields (see their note above)
  int ctxPercent = -1;
  int weekModelPercent = -1; // per-model weekly limit; -1 = absent (row hidden)
  char weekModelName[24] = "";      // e.g. "Fable"
  char weekModelResets[24] = "";
  float creditsUsed = -1;    // extra-usage dollars; -1 = unavailable
  float creditsLimit = -1;
  int creditsPercent = -1;
  uint32_t lastFetchOkMs = 0;
  double btcPrice = -1;      // BTC/USDT (from the Mac via /api/usage); -1 = unknown
  float weatherTempC = -999; // Bangkok temp (from the Mac via /api/usage); -999 = unknown
  int weatherCode = -1;      // WMO weather_code (from the Mac); -1 = unknown
  // Weather page fields — same payload as the status card's temp/code, plus
  // today's H/L, a short condition label, next 6h, and next 5d.
  // -999 / -1 / empty mean "not received yet" (show "--" / hide row).
  int weatherHigh = -999;
  int weatherLow = -999;
  char weatherCondition[20] = "";
  WeatherHour weatherHourly[WEATHER_HOURLY_N];
  uint8_t weatherHourlyCount = 0;
  WeatherDay weatherDaily[WEATHER_DAILY_N];
  uint8_t weatherDailyCount = 0;
  bool sdOk = false;
  // Written by networkTask() (core 0) without stateMutex (see state.h's lock
  // comment: only the brief result-copy takes the lock, not this flag), read
  // by loop()/presentFrame()/GIFDraw()/openCatAtIndex() on core 1 -- volatile
  // for the same cross-core-visibility reason POLL_INTERVAL_MS is, since nothing
  // here serializes it.
  volatile bool haveData = false;    // true once any data (live or SD-cached) has been applied
};
// Defined in cyd_dashboard.ino.
extern UsageState STATE;

// Page/navigation state — owned by loop()'s touch handler (cyd_dashboard.ino),
// read by pages.cpp/settings.cpp/gif_player.cpp to know what's on screen.
extern int currentPage;
extern int cfgBootPage;
// Weather detail overlay (opened by tapping the weather card on page 0).
// Not a swipe-cycle page — same pattern as settingsScreen: any tap exits.
extern bool weatherPageOpen;
// Device Stats overlay (opened by tapping the footer's CPU/ROM/RAM stats
// line -- DEVICE_HIT_*). Same not-a-swipe-page pattern as weatherPageOpen.
extern bool devicePageOpen;
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
extern uint32_t cfgShiftStepMs;  // dwell per step; flash "pixel_shift_min", 0 disables
extern uint8_t shiftIdx;
extern int shiftX, shiftY;
extern uint32_t lastShiftMs;
extern bool shiftDirty;

// CPU load estimate + touch edge tracking.
extern float cpuPercentAvg;
extern bool touchWasDown;
// Did the most recent fetch reach the server? Written by networkTask() (core
// 0), read by drawFooter()/loop()'s progress line (core 1) with no lock --
// volatile for the same cross-core-visibility reason as STATE.haveData.
extern bool wifiOk;
extern volatile bool connected;

// Guards every read/write of the shared UsageState between the render loop
// (core 1) and the network task (core 0). Non-recursive —
// draw helpers must never re-lock. See cyd_dashboard.ino's networkTask()/
// pages.cpp's render() for the two sides of this.
extern SemaphoreHandle_t stateMutex;
inline void lockState()   { if (stateMutex) xSemaphoreTake(stateMutex, portMAX_DELAY); }
inline void unlockState() { if (stateMutex) xSemaphoreGive(stateMutex); }

// Serializes the SD/HSPI bus between the two cores (networkTask on core 0,
// and the page-5 GIF player reading frames from SD on the render core, core
// 1). NON-RECURSIVE and NON-NESTABLE with stateMutex in the reverse order:
// the only nesting allowed is sdMutex -> stateMutex.
extern SemaphoreHandle_t sdMutex;
inline void lockSD()   { if (sdMutex) xSemaphoreTake(sdMutex, portMAX_DELAY); }
inline void unlockSD() { if (sdMutex) xSemaphoreGive(sdMutex); }

// Cat-shuffle interval: how long each cat GIF plays before rotating to a new
// random one. Defined in gif_player.cpp (gifTick reads it); settings.cpp's
// Cat Shuffle setting reads/writes it via getCurrentCatShuffle/applyCatShuffle.
extern uint32_t catShuffleMs;

// Runtime overrides for the compiled config.h defaults, loaded from internal
// flash (NVS, see sd_store.cpp's loadRuntimeConfig). Defined in cyd_dashboard.ino.
extern String cfgWifiSsid;
extern String cfgWifiPassword;
extern String cfgServerHost;
extern int cfgServerPort;
extern int cfgBrightness;
extern bool cfgNightModeOn;
extern bool nightDimActive;
const uint8_t NIGHT_MODE_DIM_VALUE = 64;  // ~25%, matches the brightness preset
// Battery Save: floor usage-poll interval only (no backlight change). User's
// Poll Interval preference stays stored; applyEffectivePoll() applies the floor.
// Mode is Settings OFF/ON/AUTO (persisted as flash key "battery_save"):
//   0 OFF  — never floor
//   1 ON   — always floor
//   2 AUTO — follow Mac /api/usage power.battery_save (default)
// serverBatterySave is the last *live* Mac flag (not applied from SD cache).
const int BATTERY_SAVE_OFF = 0;
const int BATTERY_SAVE_ON = 1;
const int BATTERY_SAVE_AUTO = 2;
// Written from the Settings leaf on core 1 (loop()), read from networkTask's
// applyEffectivePoll()/batterySaveActive() call chain on core 0 -- volatile
// like cfgPollIntervalSec above.
extern volatile int cfgBatterySaveMode;
extern volatile bool serverBatterySave;
const uint32_t BATTERY_SAVE_POLL_SEC = 120;   // 2 min minimum while mode is on
inline bool batterySaveActive() {
  if (cfgBatterySaveMode == BATTERY_SAVE_ON) return true;
  if (cfgBatterySaveMode == BATTERY_SAVE_AUTO) return serverBatterySave;
  return false;
}
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
// mutates its live global directly, then queues the flash key/value here;
// networkTask (core 0) drains it so the flash write never happens on the
// render core. Defined in cyd_dashboard.ino (networkTask drains it);
// queued from settings.cpp.
extern volatile bool pendingConfigSave;
extern volatile uint8_t pendingConfigKeyId;
extern volatile int32_t pendingConfigValue;
extern volatile bool pendingForgetWifi;
// Restart (Settings' RESTART row) is armed here rather than calling
// ESP.restart() directly from core 1 -- that could cut power to core 0
// mid-SD-write (under sdMutex). networkTask drains this between its own
// sequential SD operations, so the restart only ever happens once nothing
// is in flight.
extern volatile bool pendingRestart;

// Config keys persisted through the generic queue above (see
// sd_store.cpp's saveIntConfigToFlash and settings.cpp's queueConfigSave).
enum ConfigKeyId {
  CFGKEY_BRIGHTNESS = 0, CFGKEY_POLL_INTERVAL, CFGKEY_PIXEL_SHIFT, CFGKEY_BOOT_PAGE,
  CFGKEY_CAT_SHUFFLE, CFGKEY_NIGHT_MODE, CFGKEY_ROTATION, CFGKEY_SHOW_COUNTDOWN,
  CFGKEY_BATTERY_SAVE, CFGKEY_COUNT
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
// SD.totalBytes()/usedBytes() touch the HSPI bus that sdMutex serializes
// between networkTask (core 0) and the render core's cat-GIF reads (core 1)
// -- see sdMutex's contract above. refreshSdCapacityCache() does the live
// query under lockSD()/unlockSD() and must only be called from networkTask;
// drawDevicePage() (core 1, called from inside render()'s stateMutex-held
// section, so it cannot also take sdMutex -- only sdMutex -> stateMutex
// nesting is allowed, never the reverse) reads the cached result instead via
// cachedSdCapacityPercent(), no lock needed.
void refreshSdCapacityCache();
int cachedSdCapacityPercent(uint64_t &usedOut, uint64_t &totalOut);

// ── NETWORK (net.cpp) ──────────────────────────────────────
void ensureMdns();
void connectWifi();
bool resolveServer();
bool fetchUsage();
bool loadCachedUsage();
void loadEnvCache();
void loadWeatherCache();
// fromNetwork=true only for a live /api/usage fetch — applies power.battery_save
// into serverBatterySave. SD cache loads pass false so a stale Mac power flag
// can't floor the poll across an overnight offline boot.
bool applyUsageJson(const String& payload, bool fromNetwork = false);
// Set false by networkTask() (cyd_dashboard.ino) on WiFi loss, so ensureMdns()
// re-initializes once WiFi returns.
extern bool mdnsStarted;

// ── SD STORE (sd_store.cpp) ────────────────────────────────
// Runtime settings/config now persist to internal flash (NVS, via
// Preferences) rather than the SD card -- see loadRuntimeConfig()'s comment
// in sd_store.cpp. SD is still used here for diagnostics and the boot splash.
void loadRuntimeConfig();
const char* resetReasonStr();
void logDiag(const char* event);
bool drawBmpFromSD(const char* path, int dx, int dy);
void showBootSplash();
void saveWifiCredsToFlash(const String& ssid, const String& password);
void saveIntConfigToFlash(const char* key, int32_t value);
void forgetWifiFromFlash();

// ── PAGES / RENDER (pages.cpp) ─────────────────────────────
void render();
void drawOfflineBanner();
void drawMixedPageStatic();
void drawBatterySaveIcon();

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
void queueConfigSave(uint8_t keyId, int32_t value);
// Recompute live backlight (night mode) / poll cadence (battery save). Call
// after any of those inputs change (and once after loadRuntimeConfig).
void applyEffectiveBrightness();
void applyEffectivePoll();
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
