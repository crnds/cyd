// Claude Code token usage dashboard for the ESP32-2432S028R (CYD).
// Polls a local Python server for usage stats and renders 8 tap-to-cycle pages.
//
// This file now holds only the display/state object definitions, setup(),
// loop(), and networkTask() — everything else was split (2026) into
// format.cpp, net.cpp, sd_store.cpp, pages.cpp, gif_player.cpp, settings.cpp,
// and ap_setup.cpp, all declared through state.h. See state.h's header
// comment for why that split needs a shared header at all: Arduino's
// auto-prototype generation only covers this .ino, not the other .cpp files.
#include "state.h"

// ── DISPLAY OBJECTS ────────────────────────────────────────
// Declaration order here fixes their construction order within this one
// translation unit (frame's ctor just stores &gfx, so it doesn't actually
// need gfx fully constructed first, but keeping all four together sidesteps
// any static-init-order question entirely).
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

// ── STATE ──────────────────────────────────────────────────
// Non-const: overridable from /config.json (see sd_store.cpp's
// loadRuntimeConfig). Originally set once at boot before the two tasks
// start; now also settable at runtime from the Settings page's Poll Interval
// leaf (loop(), core 1), so it's volatile -- networkTask (core 0) reads it
// every cycle (see settings.cpp's applyPollInterval).
volatile uint32_t POLL_INTERVAL_MS = 20000;

UsageState STATE;

// 30-day on-device history, backed by /daily_log.csv on the SD card so it
// survives independent of the Mac server's log retention/uptime. Kept as
// plain globals (not in UsageState) since they're loaded once at boot and
// shifted in place, not part of the per-poll refresh cycle.
int64_t longTrend[LONG_TREND_DAYS];
int longTrendCount = 0;  // valid entries, oldest at [0]; may be < LONG_TREND_DAYS early on

int currentPage = 0;
int cfgBootPage = 0;  // which page currentPage starts on; overridable via /config.json "boot_page"
SettingsScreen settingsScreen = SET_OFF;
int settingsScrollOffset = 0;  // vertical scroll position (px) of the SET_LIST list
int settingsLeafIndex = -1;  // index into SETTINGS[] currently open in SET_LEAF
uint32_t confirmArmedMs = 0;
int confirmArmedRow = -1;
bool mixedPageDirty = false;
volatile uint32_t lastPollMs = 0;
uint32_t lastTouchMs = 0;

// ── PIXEL SHIFT (anti image-retention) ─────────────────────
uint32_t cfgShiftStepMs = 180000;  // dwell per step; /config.json "pixel_shift_min", 0 disables
uint8_t shiftIdx = 0;
int shiftX = 0, shiftY = 0;
uint32_t lastShiftMs = 0;
bool shiftDirty = false;  // set on step; the mixed page's partial pushes need one full re-push

// CPU load estimate: no FreeRTOS runtime-stats are enabled in the Arduino
// build, so there's no true per-core utilization API. Instead we measure the
// fraction of each loop() iteration spent doing work vs. the fixed 30ms
// delay() at its end, and smooth it with an EMA — a duty-cycle proxy for
// "how busy is the loop", not a real scheduler-level CPU% figure.
float cpuPercentAvg = 0;
bool touchWasDown = false;
bool wifiOk = false;
volatile bool connected = false;  // did the most recent fetch reach the server?

// Guards every read/write of the shared UsageState (and longTrend[]) between
// the render loop (core 1) and the network task (core 0). See state.h for
// the locking contract.
SemaphoreHandle_t stateMutex = nullptr;
SemaphoreHandle_t sdMutex = nullptr;

// Runtime overrides for the compiled config.h defaults, loaded from an
// optional /config.json on the SD card (see sd_store.cpp's loadRuntimeConfig()).
String cfgWifiSsid = WIFI_SSID;
String cfgWifiPassword = WIFI_PASSWORD;
String cfgServerHost = SERVER_HOST;
int cfgServerPort = SERVER_PORT;
int cfgBrightness = 200;   // 0-255 panel backlight; overridable via /config.json
// Night mode: fixed 23:00-07:00 schedule (Bangkok has no DST, so tm_hour is
// already correct local time -- no timezone math needed), dims to a fixed
// 25% while on. Checked once/sec in loop() (core-1-only, like cfgShiftStepMs)
// regardless of page/catMode/settings state, so it doesn't need a volatile
// -- nightDimActive tracks whether the dim is currently applied, so
// applyNightMode() can restore immediately if toggled off mid-dim.
bool cfgNightModeOn = false;
bool nightDimActive = false;
// Generic Settings-page persistence queue: a leaf's apply() (loop(), core 1)
// mutates its live global directly, then queues the /config.json key/value
// here; networkTask (core 0) drains it so the SD write never happens on the
// render core. One queue serves every single-int setting (brightness, poll
// interval, pixel-shift, etc.) -- see settings.cpp's CONFIG_KEY_NAMES/
// queueConfigSave().
volatile bool pendingConfigSave = false;
volatile uint8_t pendingConfigKeyId = 0;
volatile int32_t pendingConfigValue = 0;
// Forget WiFi doesn't fit the single-int queue above (it erases two *string*
// keys), and its restart must wait for the SD write to actually finish, so
// it gets its own flag -- networkTask() does the erase then restarts.
volatile bool pendingForgetWifi = false;
int cfgScreenRotation = 1;
int cfgTouchXMin = 200;
int cfgTouchXMax = 3800;
int cfgTouchYMin = 3800;   // inverted on user's board (drives left/right at rotation 1)
int cfgTouchYMax = 200;
int cfgTouchOffsetRotation = 0;

// Server discovery: when SERVER_HOST ends in ".local" net.cpp's
// resolveServer() looks it up via mDNS. mdnsStarted is reset here (core 0)
// whenever WiFi drops, so ensureMdns() (net.cpp) re-initializes once it's
// back — the board's own IP may have changed across the outage.
int wifiDownCycles = 0;
const int RESTART_AFTER_CYCLES = 45;  // ~15 min of no WiFi -> self-reboot

// Consecutive failed polls (WiFi down OR WiFi up but the Mac unreachable)
// before STATE.haveData is forced back to false, switching the display to the
// cat-GIF/OFFLINE screen. STATE.haveData is otherwise sticky-true once any
// fetch (or SD cache load) succeeds, so this is what makes a real outage
// actually visible instead of just freezing the last-known dashboard forever.
int fetchFailCycles = 0;
const int OFFLINE_AFTER_CYCLES = 3;  // ~1 min at the default 20s poll interval

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

    if (pendingConfigSave) {
      pendingConfigSave = false;
      saveIntConfigToSD(CONFIG_KEY_NAMES[pendingConfigKeyId], pendingConfigValue);
    }

    if (pendingForgetWifi) {
      pendingForgetWifi = false;
      forgetWifiFromSD();
      ESP.restart();  // only after the erase above has actually landed on SD
    }

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
  randomSeed(esp_random());           // so the cat picked on the cat pages differs each boot

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
    currentPage = cfgBootPage;  // honor the /config.json "boot_page" override
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

  // First-boot config portal: only when there's an SD card to persist the
  // result to and no WiFi SSID has ever been configured. See ap_setup.cpp's
  // runApSetup().
  if (STATE.sdOk && cfgWifiSsid.length() == 0) {
    runApSetup();  // blocks until configured, then restarts -- never returns
  }

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

  // Cats own the screen on the cat pages, AND whenever offline (the cat GIF loop
  // doubles as the offline screen, with an OFFLINE banner overlaid on top —
  // see drawOfflineBanner()/gifTick()).
  bool offline = !STATE.haveData;
  bool catMode = (currentPage == GIF_PAGE) || (currentPage == MIXED_PAGE) || offline;

  // Anti-retention pixel shift. In cat mode the resulting shiftDirty is
  // consumed by gifTick's own presentFrame (placeholder included); on pages
  // 0-4 the render() below repaints at the new offset right away.
  pixelShiftTick(now);

  // Night mode: applies regardless of page/catMode/settings state, on its
  // own 1s timer (decoupled from the render cadence below, which is skipped
  // in catMode/settings). Only calls setBrightness() on a transition edge.
  if (cfgNightModeOn) {
    static uint32_t lastNightCheckMs = 0;
    if (now - lastNightCheckMs >= 1000) {
      lastNightCheckMs = now;
      struct tm ti;
      if (getLocalTime(&ti, 0)) {
        bool inWindow = (ti.tm_hour >= 23 || ti.tm_hour < 7);
        if (inWindow && !nightDimActive) {
          gfx.setBrightness(NIGHT_MODE_DIM_VALUE);
          nightDimActive = true;
        } else if (!inWindow && nightDimActive) {
          gfx.setBrightness(cfgBrightness);
          nightDimActive = false;
        }
      }
    }
  }

  // catMode-transition bookkeeping: enter/exit the GIF decoder via
  // gif_player.cpp's wrapper functions (it owns the AnimatedGIF instance and
  // its ~24KB buffers now, not this file).
  static bool prevCatMode = false;
  if (catMode != prevCatMode) {
    if (!catMode) {
      gifPlayerExitCatMode();
    } else {
      gifPlayerEnterCatMode();
    }
    prevCatMode = catMode;
  }

  if (settingsScreen != SET_OFF) {
    // Static page: no clock/session countdown to tick, no progress line, no
    // GIF playback -- drawn once on entry/change (see renderSettings() calls
    // inside handleSettingsTouch()), not on a periodic cadence.
  } else if (catMode) {
    // Cats animate frame-by-frame; they own the whole screen (no footer or
    // poll-progress line) and need no 1s repaint.
    gifTick(offline);
    if (currentPage == MIXED_PAGE && !offline) {
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
    if (now - lastRenderMs >= 1000 || shiftDirty) {
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
      int w = (int)((float)elapsed / POLL_INTERVAL_MS * 304);
      if (w < lineW) {
        lineW = w;  // new poll cycle: the full repaint already cleared the line
      } else if (w > lineW) {
        bool isEvenSecond = false;
        bool isFlashWindow = checkHourlyFlash(isEvenSecond);
        if (!(isFlashWindow && isEvenSecond)) {
          // Direct panel write, bypasses presentFrame — apply the pixel-shift
          // offset itself so it lands on the same row as the blitted frame.
          gfx.fillRect(lineW + shiftX, 239 + shiftY, w - lineW, 1, COL_TEXT2);
        }
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

    if (settingsScreen == SET_LEAF) {
      handleSettingsTouch(tx, ty, now, catMode);
    } else if (settingsScreen == SET_LIST) {
      settingsListDragBegin(tx, ty);  // resolved as a tap or a scroll on release, below
    } else if (!catMode && tx >= PULSE_HIT_X0 && tx < PULSE_HIT_X1 &&
               ty >= PULSE_HIT_Y0 && ty < PULSE_HIT_Y1) {
      settingsScreen = SET_LIST;
      settingsScrollOffset = 0;
      renderSettings();
    } else {
      bool isRight = (tx >= 152);
      if (isRight) {
        currentPage = (currentPage + 1) % PAGE_COUNT;
      } else {
        currentPage = (currentPage - 1 + PAGE_COUNT) % PAGE_COUNT;
      }

      // Handle initialization when entering a page
      if (currentPage == MIXED_PAGE && !offline) {
        lockState();
        g->fillScreen(COL_BG);
        drawMixedPageStatic();
        unlockState();
        presentFrame();
        gifPlayerResetForPageChange();
      } else if (currentPage == GIF_PAGE) {
        gifPlayerResetForPageChange();
      }

      if (!catMode) render();  // catMode: gifTick() already redraws every pass
      flashTouchBorder(isRight);  // one-frame white edge flash on the new page: touch registered
    }
  } else if (touchDown && touchWasDown && settingsScreen == SET_LIST) {
    settingsListDragMove(tx, ty);  // live-scroll while the finger stays down, no debounce gate
  } else if (!touchDown && touchWasDown && settingsScreen == SET_LIST) {
    settingsListDragEnd(catMode);  // tap (open a leaf / exit) vs scroll, decided from total movement
  }
  touchWasDown = touchDown;

  // Duty-cycle CPU estimate: work time this iteration vs. the fixed 30ms
  // delay below, smoothed with an EMA (see cpuPercentAvg declaration).
  uint32_t busyUs = micros() - loopStartUs;
  float sample = (float)busyUs / (busyUs + 30000.0f) * 100.0f;
  cpuPercentAvg = cpuPercentAvg * 0.9f + sample * 0.1f;

  delay(30);
}
