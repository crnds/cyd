// Settings area (tap the footer's gear icon to enter). A generic
// two-screen system: SET_LIST is a scrollable list of setting names
// (drawSettingsList), SET_LEAF is a value-picker grid for whichever one is
// open (drawSettingsLeaf). Every setting is one SettingDef row -- a label, a
// small set of {value,label} options, and two plain function pointers (no
// std::function/virtual dispatch -- flash is scarce here) -- so adding a
// setting is a data row + a short apply()/getCurrent() pair, not a
// hand-copied page. Split out of cyd_dashboard.ino.
#include "state.h"

struct SettingDef {
  const char* label;      // list-row title
  const char* leafTitle;  // leaf header (big title)
  const char* subtitle;   // leaf subheading under the title; "" to omit
  const char* hint;       // leaf hint line at the bottom
  uint8_t cols, rows, count;   // button grid shape; rows*cols >= count
  const int* values;           // raw option values, length == count
  const char* const* valueLabels;  // button text, length == count
  uint8_t btnTextSize;    // 1 or 2, sized to fit the longest valueLabel
  bool destructive;       // reserved for confirm-arm actions (Tier 1+)
  int (*getCurrent)();        // value to highlight as "on"
  void (*apply)(int value);   // live mutation + queues SD persistence
};

static const int BRIGHTNESS_VALUES[5] = {0, 64, 128, 191, 255};
static const char* const BRIGHTNESS_LABELS[5] = {"0%", "25%", "50%", "75%", "100%"};

// Config keys persisted through the generic queue (see pendingConfigSave in
// state.h and sd_store.cpp's saveIntConfigToFlash()/cyd_dashboard.ino's
// networkTask()). Index here must match the id passed to queueConfigSave()
// from each setting's apply(). These are also the NVS key names used in
// flash (Preferences), which caps key length at 15 chars -- poll_sec and
// night_mode are shortened from their old /config.json names for that limit.
const char* const CONFIG_KEY_NAMES[CFGKEY_COUNT] = {
  "brightness", "poll_sec", "pixel_shift_min", "boot_page", "cat_shuffle_sec",
  "night_mode", "screen_rotation", "show_countdown", "battery_save", "show_aqi"
};

void queueConfigSave(uint8_t keyId, int32_t value) {
  pendingConfigKeyId = keyId;
  pendingConfigValue = value;
  pendingConfigSave = true;
}

// Live backlight from user preference + night-mode overlay only.
// Battery Save never touches brightness.
void applyEffectiveBrightness() {
  if (nightDimActive) {
    gfx.setBrightness(NIGHT_MODE_DIM_VALUE);
    return;
  }
  gfx.setBrightness((uint8_t)cfgBrightness);
}

// Live poll cadence: user's Poll Interval, floored to 2 min while Battery
// Save is active (Settings ON, or AUTO + Mac power.battery_save). networkTask
// only reads POLL_INTERVAL_MS.
void applyEffectivePoll() {
  uint32_t sec = cfgPollIntervalSec;
  if (sec < 5) sec = 5;
  if (sec > 3600) sec = 3600;
  if (batterySaveActive() && sec < BATTERY_SAVE_POLL_SEC) sec = BATTERY_SAVE_POLL_SEC;
  POLL_INTERVAL_MS = sec * 1000;
}

static int getCurrentBrightness() { return cfgBrightness; }
static void applyBrightness(int v) {
  cfgBrightness = (uint8_t)v;
  applyEffectiveBrightness();  // may stay dimmed under night mode
  queueConfigSave(CFGKEY_BRIGHTNESS, v);
}

// Destructive/action rows (Restart, and later Forget WiFi) have no "current
// value" to highlight -- drawSettingsLeaf() skips getCurrent() for them
// entirely via def.destructive, but the function pointer still needs a body.
static int getCurrentNone() { return -1; }

static const int ACTION_VALUES[1] = {0};  // dummy value: these rows are actions, not options
// Arms the restart rather than calling ESP.restart() directly here on core 1
// -- that could cut power to core 0 mid-SD-write; networkTask drains
// pendingRestart between its own sequential SD operations instead.
static void applyRestart(int v) { pendingRestart = true; }
static const char* const RESTART_LABELS[1] = {"RESTART"};

// Erasing the flash-saved WiFi creds + the restart-into-AP-portal must both
// happen on networkTask (core 0) -- see forgetWifiFromFlash()/pendingForgetWifi
// -- so apply() here just arms the flag and returns immediately.
static void applyForgetWifi(int v) { pendingForgetWifi = true; }
static const char* const FORGET_WIFI_LABELS[1] = {"FORGET WIFI"};

// Poll interval: how often networkTask fetches /api/usage from the Mac.
// Values are seconds (matches the "poll_sec" NVS key directly, no unit
// conversion needed at the flash-persistence layer).
static const int POLL_VALUES[5] = {5, 10, 20, 60, 300};
static const char* const POLL_LABELS[5] = {"5s", "10s", "20s", "60s", "5m"};
// Leaf shows the *user* preference, not the Battery-Save-stretched effective
// rate — otherwise turning Battery Save on would make the Poll leaf look like
// the user had picked 2m.
static int getCurrentPollInterval() { return (int)cfgPollIntervalSec; }
static void applyPollInterval(int v) {
  cfgPollIntervalSec = (uint32_t)v;
  applyEffectivePoll();  // networkTask reads the (possibly floored) POLL_INTERVAL_MS
  queueConfigSave(CFGKEY_POLL_INTERVAL, v);
}

// Anti-retention pixel shift: cfgShiftStepMs is read only by pixelShiftTick(),
// which is only called from loop() -- core-1-only, so this can be mutated
// directly with no volatile/queue needed for the live value (unlike poll
// interval, which crosses to networkTask on core 0).
static const int PIXEL_SHIFT_VALUES[4] = {0, 1, 3, 10};
static const char* const PIXEL_SHIFT_LABELS[4] = {"OFF", "1m", "3m", "10m"};
static int getCurrentPixelShift() { return (int)(cfgShiftStepMs / 60000); }
static void applyPixelShift(int v) {
  cfgShiftStepMs = (uint32_t)v * 60000;
  queueConfigSave(CFGKEY_PIXEL_SHIFT, v);
}

// Boot page: which of the 6 pages currentPage starts on next boot. All 6
// (including the cat/mixed pages) are valid -- render()'s switch and loop()'s
// catMode check already handle those generically regardless of how
// currentPage got set, no special-casing needed. Device Stats isn't in this
// list -- it's an overlay (devicePageOpen), not a currentPage value.
static const int PAGE_VALUES[5] = {0, 1, 2, 3, 4};
static const char* const PAGE_LABELS[5] = {
  "STATUS", "PROJECTS", "LIMITS", "CATS", "MIXED"
};
static int getCurrentBootPage() { return cfgBootPage; }
static void applyBootPage(int v) {
  cfgBootPage = v;
  currentPage = v;  // jump the live dashboard too -- a free, immediate effect
  queueConfigSave(CFGKEY_BOOT_PAGE, v);
}

// Cat shuffle interval: how long each cat GIF plays before rotating to a new
// random one. catShuffleMs lives in gif_player.cpp (core-1-only, read in
// gifTick()), so no volatile/queue needed for the live value here.
static const int CAT_SHUFFLE_VALUES[4] = {0, 5, 10, 30};
static const char* const CAT_SHUFFLE_LABELS[4] = {"OFF", "5s", "10s", "30s"};
static int getCurrentCatShuffle() { return (int)(catShuffleMs / 1000); }
static void applyCatShuffle(int v) {
  catShuffleMs = (uint32_t)v * 1000;
  queueConfigSave(CFGKEY_CAT_SHUFFLE, v);
}

static const int NIGHT_MODE_VALUES[2] = {0, 1};
static const char* const NIGHT_MODE_LABELS[2] = {"OFF", "ON"};
static int getCurrentNightMode() { return cfgNightModeOn ? 1 : 0; }
static void applyNightMode(int v) {
  cfgNightModeOn = (v != 0);
  if (!cfgNightModeOn && nightDimActive) {
    nightDimActive = false;
    applyEffectiveBrightness();  // restore user brightness
  }
  queueConfigSave(CFGKEY_NIGHT_MODE, v);
}

// Battery Save: stretches short usage polls to 2 min only (no backlight
// change). OFF / ON / AUTO — AUTO follows Mac /api/usage power.battery_save
// (control panel toggle or unplug). User's Poll Interval is preserved.
static const int BATTERY_SAVE_VALUES[3] = {
  BATTERY_SAVE_OFF, BATTERY_SAVE_ON, BATTERY_SAVE_AUTO
};
static const char* const BATTERY_SAVE_LABELS[3] = {"OFF", "ON", "AUTO"};
static int getCurrentBatterySave() { return cfgBatterySaveMode; }
static void applyBatterySave(int v) {
  if (v < BATTERY_SAVE_OFF) v = BATTERY_SAVE_OFF;
  if (v > BATTERY_SAVE_AUTO) v = BATTERY_SAVE_AUTO;
  cfgBatterySaveMode = v;
  applyEffectivePoll();
  queueConfigSave(CFGKEY_BATTERY_SAVE, v);
}

// Rotation flip: 1 = normal, 3 = 180 degrees (both are landscape; ILI9341
// rotation 0-3 = 0/90/180/270). Applies live, no restart -- confirmed by
// reading LovyanGFX's Panel_Device::convertRawXY()/setCalibrate(): the touch
// affine transform is built once from panel_width/panel_height and the touch
// x_min/x_max/y_min/y_max (all rotation-independent), while a *separate*
// per-touch-read step recomputes `r = (panel_rotation + touch.offset_rotation)
// & 3` and swaps/flips the already-transformed point accordingly -- so touch
// automatically tracks any setRotation() change with the calibration values
// completely untouched. No dedicated multi-key save function needed: only
// screen_rotation actually changes, so this reuses the generic int queue.
static const int ROTATION_VALUES[2] = {1, 3};
static const char* const ROTATION_LABELS[2] = {"NORMAL", "FLIPPED"};
static int getCurrentRotation() { return cfgScreenRotation; }
static void applyRotation(int v) {
  cfgScreenRotation = v;
  gfx.applyRuntimeConfig(cfgScreenRotation, cfgTouchXMin, cfgTouchXMax, cfgTouchYMin, cfgTouchYMax, cfgTouchOffsetRotation);
  queueConfigSave(CFGKEY_ROTATION, v);
}

// Show Countdown: green progress bars under the 5h/week usage bars on the
// status/mixed limits card, plus the translucent pie wedge on the analog
// clock (hour hand -> next 5h reset). The thin green reset hand on the clock
// always stays when a reset time is known. Default ON.
static const int SHOW_COUNTDOWN_VALUES[2] = {0, 1};
static const char* const SHOW_COUNTDOWN_LABELS[2] = {"OFF", "ON"};
static int getCurrentShowCountdown() { return cfgShowCountdown ? 1 : 0; }
static void applyShowCountdown(int v) {
  cfgShowCountdown = (v != 0);
  queueConfigSave(CFGKEY_SHOW_COUNTDOWN, v);
}

static const int SHOW_AQI_VALUES[2] = {0, 1};
static const char* const SHOW_AQI_LABELS[2] = {"OFF", "ON"};
static int getCurrentShowAqi() { return cfgShowAqi ? 1 : 0; }
static void applyShowAqi(int v) {
  cfgShowAqi = (v != 0);
  queueConfigSave(CFGKEY_SHOW_AQI, v);
}

static const SettingDef SETTINGS[] = {
  { "BRIGHTNESS", "BRIGHTNESS", "BACKLIGHT BRIGHTNESS", "TAP A LEVEL TO APPLY",
    5, 1, 5, BRIGHTNESS_VALUES, BRIGHTNESS_LABELS, 2, false,
    getCurrentBrightness, applyBrightness },
  { "POLL INTERVAL", "POLL INTERVAL", "HOW OFTEN TO FETCH /API/USAGE", "TAP A RATE TO APPLY",
    5, 1, 5, POLL_VALUES, POLL_LABELS, 2, false,
    getCurrentPollInterval, applyPollInterval },
  { "PIXEL SHIFT", "PIXEL SHIFT", "ANTI-RETENTION ORBIT INTERVAL", "TAP A RATE TO APPLY",
    4, 1, 4, PIXEL_SHIFT_VALUES, PIXEL_SHIFT_LABELS, 2, false,
    getCurrentPixelShift, applyPixelShift },
  { "BOOT PAGE", "BOOT PAGE", "PAGE SHOWN AFTER POWER-ON", "TAP A PAGE TO APPLY",
    3, 2, 5, PAGE_VALUES, PAGE_LABELS, 1, false,
    getCurrentBootPage, applyBootPage },
  { "RESTART", "RESTART", "", "TAP TWICE TO RESTART THE BOARD",
    1, 1, 1, ACTION_VALUES, RESTART_LABELS, 2, true,
    getCurrentNone, applyRestart },
  { "FORGET WIFI", "FORGET WIFI", "", "TAP TWICE TO ERASE WIFI CREDS",
    1, 1, 1, ACTION_VALUES, FORGET_WIFI_LABELS, 2, true,
    getCurrentNone, applyForgetWifi },
  { "CAT SHUFFLE", "CAT SHUFFLE", "HOW LONG EACH CAT GIF PLAYS", "TAP A RATE TO APPLY",
    4, 1, 4, CAT_SHUFFLE_VALUES, CAT_SHUFFLE_LABELS, 2, false,
    getCurrentCatShuffle, applyCatShuffle },
  { "NIGHT MODE", "NIGHT MODE", "23:00-07:00, DIMS TO 25%", "TAP TO TOGGLE",
    2, 1, 2, NIGHT_MODE_VALUES, NIGHT_MODE_LABELS, 2, false,
    getCurrentNightMode, applyNightMode },
  { "BATTERY SAVE", "BATTERY SAVE", "2MIN POLL; AUTO = FOLLOW MAC", "TAP A MODE TO APPLY",
    3, 1, 3, BATTERY_SAVE_VALUES, BATTERY_SAVE_LABELS, 2, false,
    getCurrentBatterySave, applyBatterySave },
  { "ROTATION", "ROTATION", "FOR UPSIDE-DOWN MOUNTING", "APPLIES IMMEDIATELY",
    2, 1, 2, ROTATION_VALUES, ROTATION_LABELS, 1, false,
    getCurrentRotation, applyRotation },
  { "SHOW COUNTDOWN", "SHOW COUNTDOWN", "GREEN BARS + CLOCK WEDGE", "TAP TO TOGGLE",
    2, 1, 2, SHOW_COUNTDOWN_VALUES, SHOW_COUNTDOWN_LABELS, 2, false,
    getCurrentShowCountdown, applyShowCountdown },
  { "SHOW AQI", "SHOW AQI", "BADGE NEXT TO THE STATUS DATE", "TAP TO TOGGLE",
    2, 1, 2, SHOW_AQI_VALUES, SHOW_AQI_LABELS, 2, false,
    getCurrentShowAqi, applyShowAqi },
};
static const int SETTINGS_COUNT = 12;

static const int SET_BACK_X0 = 0, SET_BACK_X1 = 100, SET_BACK_Y0 = 0, SET_BACK_Y1 = 34;
static const int SET_BTN_X0 = 11, SET_BTN_Y = 100, SET_BTN_W = 54, SET_BTN_H = 56;
static const int SET_BTN_STEP = 58, SET_BTN_STEP_Y = 62;  // STEP_Y only matters for rows>1
static const int SET_ROW_X0 = 10, SET_ROW_Y0 = 34, SET_ROW_W = 302, SET_ROW_H = 46, SET_ROW_STEP = 52;
// A lone action button (count==1, e.g. Restart/Forget WiFi) gets the full
// row width instead of one narrow preset-grid cell -- "TAP AGAIN" doesn't
// fit in a 54px-wide button, and a wide button reads better for an action
// anyway. Shares its x/width with the list rows for visual consistency.
static const int SET_WIDE_BTN_X0 = SET_ROW_X0, SET_WIDE_BTN_W = SET_ROW_W;

// The list scrolls rather than paginates: SETTINGS_VISIBLE_ROWS worth of rows
// show in a fixed viewport below the header, and settingsScrollOffset (px)
// slides the full SETTINGS_COUNT-row list through it. Row i's unscrolled y is
// SETTINGS_VIEWPORT_Y0 + i*SET_ROW_STEP; SETTINGS_CONTENT_H/SETTINGS_SCROLL_MAX
// are the compile-time totals used to clamp the drag and size the scrollbar
// thumb below.
static const int SETTINGS_VISIBLE_ROWS = 4;
static const int SETTINGS_VIEWPORT_Y0 = SET_ROW_Y0;
static const int SETTINGS_VIEWPORT_H = SETTINGS_VISIBLE_ROWS * SET_ROW_STEP - (SET_ROW_STEP - SET_ROW_H);
static const int SETTINGS_CONTENT_H = SETTINGS_COUNT * SET_ROW_STEP - (SET_ROW_STEP - SET_ROW_H);
static const int SETTINGS_SCROLL_MAX = (SETTINGS_CONTENT_H > SETTINGS_VIEWPORT_H)
    ? (SETTINGS_CONTENT_H - SETTINGS_VIEWPORT_H) : 0;
static const int SETTINGS_SCROLLBAR_X = 316, SETTINGS_SCROLLBAR_W = 4;

static void drawSettingsList() {
  g->fillScreen(COL_BG);

  g->setTextColor(COL_ACCENT);
  g->setTextSize(2);
  g->setCursor(10, 10);
  g->print("X");

  // Shares the header line with "X" now (was its own textSize(3) line above
  // the rows) -- freed-up height goes to the row viewport below instead.
  // "SETTINGS" is 8 chars * 6px/char at size 1 = 48px wide; right-aligned
  // flush against the screen's right edge.
  g->setTextColor(COL_ACCENT);
  g->setTextSize(1);
  g->setCursor(320 - 48, 14);
  g->print("SETTINGS");

  g->setClipRect(0, SETTINGS_VIEWPORT_Y0, 320, SETTINGS_VIEWPORT_H);
  for (int idx = 0; idx < SETTINGS_COUNT; idx++) {
    int y = SETTINGS_VIEWPORT_Y0 + idx * SET_ROW_STEP - settingsScrollOffset;
    if (y + SET_ROW_H < SETTINGS_VIEWPORT_Y0 || y > SETTINGS_VIEWPORT_Y0 + SETTINGS_VIEWPORT_H) continue;
    g->fillRoundRect(SET_ROW_X0, y, SET_ROW_W, SET_ROW_H, 6, COL_SURFACE);
    g->drawRoundRect(SET_ROW_X0, y, SET_ROW_W, SET_ROW_H, 6, COL_BORDER);
    g->setTextColor(COL_TEXT);
    g->setTextSize(2);
    g->setCursor(SET_ROW_X0 + 12, y + (SET_ROW_H - 16) / 2);
    g->print(SETTINGS[idx].label);
    g->setTextColor(COL_ACCENT);
    g->setCursor(SET_ROW_X0 + SET_ROW_W - 24, y + (SET_ROW_H - 16) / 2);
    g->print(">");
  }
  g->clearClipRect();

  if (SETTINGS_SCROLL_MAX > 0) {
    g->fillRoundRect(SETTINGS_SCROLLBAR_X, SETTINGS_VIEWPORT_Y0, SETTINGS_SCROLLBAR_W, SETTINGS_VIEWPORT_H, 2, COL_BORDER);
    int thumbH = SETTINGS_VIEWPORT_H * SETTINGS_VIEWPORT_H / SETTINGS_CONTENT_H;
    if (thumbH < 16) thumbH = 16;
    int thumbY = SETTINGS_VIEWPORT_Y0 +
        (SETTINGS_VIEWPORT_H - thumbH) * settingsScrollOffset / SETTINGS_SCROLL_MAX;
    g->fillRoundRect(SETTINGS_SCROLLBAR_X, thumbY, SETTINGS_SCROLLBAR_W, thumbH, 2, COL_ACCENT);
  }
}

static void drawSettingsLeaf() {
  const SettingDef& def = SETTINGS[settingsLeafIndex];
  g->fillScreen(COL_BG);

  g->setTextColor(COL_ACCENT);
  g->setTextSize(2);
  g->setCursor(10, 10);
  g->print("< BACK");

  g->setTextColor(COL_ACCENT);
  g->setTextSize(3);
  g->setCursor(10, 40);
  g->print(def.leafTitle);

  if (def.subtitle[0]) {
    g->setTextColor(COL_TEXT2);
    g->setTextSize(1);
    g->setCursor(10, 76);
    g->print(def.subtitle);
  }

  // Only lit when the current value is an exact preset -- a value migrated
  // from an older config or otherwise off-preset just shows no selection
  // until the user taps one, rather than lying about which preset is
  // "closest". Destructive
  // rows have no "current value" -- they're armed/unarmed instead (see
  // confirmArmedRow), so getCurrent() isn't even called for them.
  int current = def.destructive ? -1 : def.getCurrent();
  bool armed = def.destructive && confirmArmedRow == settingsLeafIndex &&
               (millis() - confirmArmedMs < CONFIRM_ARM_MS);
  int btnW = (def.count == 1) ? SET_WIDE_BTN_W : SET_BTN_W;
  for (int i = 0; i < def.count; i++) {
    int col = i % def.cols;
    int row = i / def.cols;
    int x = (def.count == 1) ? SET_WIDE_BTN_X0 : SET_BTN_X0 + col * SET_BTN_STEP;
    int y = SET_BTN_Y + row * SET_BTN_STEP_Y;
    bool on = armed || (!def.destructive && def.values[i] == current);
    uint16_t fill = armed ? COL_WARN : (on ? COL_ACCENT : COL_SURFACE);
    uint16_t border = armed ? COL_WARN : (on ? COL_ACCENT : COL_BORDER);
    g->fillRoundRect(x, y, btnW, SET_BTN_H, 6, fill);
    g->drawRoundRect(x, y, btnW, SET_BTN_H, 6, border);
    const char* label = armed ? "TAP AGAIN" : def.valueLabels[i];
    int lw = (int)strlen(label) * 6 * def.btnTextSize;
    g->setTextColor(on ? COL_BG : COL_TEXT);
    g->setTextSize(def.btnTextSize);
    g->setCursor(x + (btnW - lw) / 2, y + (SET_BTN_H - 8 * def.btnTextSize) / 2);
    g->print(label);
  }

  g->setTextColor(COL_TEXT2);
  g->setTextSize(1);
  int hw = (int)strlen(def.hint) * 6;
  int hintY = (def.rows > 1) ? 230 : 168;  // clears the 2-row grid (e.g. Boot Page)
  g->setCursor((320 - hw) / 2, hintY);
  g->print(def.hint);
}

void renderSettings() {
  // Neither screen touches STATE, so no stateMutex needed here.
  if (settingsScreen == SET_LEAF) drawSettingsLeaf();
  else drawSettingsList();
  presentFrame();
}

// True only between a real settingsListDragBegin() and its matching End/abandon
// (see the drag-gesture trio below) -- declared up here too since
// handleSettingsTouch's BACK case needs to clear it.
static bool dragActive = false;

// Touch handling for SET_LEAF (called from loop() on a fresh debounced tap,
// same as before). SET_LIST no longer routes through here at all -- see the
// drag-gesture trio below.
bool handleSettingsTouch(int32_t tx, int32_t ty, uint32_t now, bool catMode) {
  const SettingDef& def = SETTINGS[settingsLeafIndex];
  if (tx >= SET_BACK_X0 && tx < SET_BACK_X1 && ty >= SET_BACK_Y0 && ty < SET_BACK_Y1) {
    confirmArmedRow = -1;
    settingsScreen = SET_LIST;
    // This same physical touch is still down and will generate Move/End calls
    // against the list next -- but it already did its job (leaf -> list) via
    // this tap, so it must NOT also be replayed as a list tap/scroll on release
    // (that previously reopened this same leaf, or -- since BACK's hit-box is
    // the list's own close-box coords -- could otherwise close Settings outright).
    dragActive = false;
    renderSettings();
    return true;
  }
  int btnW = (def.count == 1) ? SET_WIDE_BTN_W : SET_BTN_W;
  for (int i = 0; i < def.count; i++) {
    int col = i % def.cols;
    int row = i / def.cols;
    int x = (def.count == 1) ? SET_WIDE_BTN_X0 : SET_BTN_X0 + col * SET_BTN_STEP;
    int y = SET_BTN_Y + row * SET_BTN_STEP_Y;
    if (tx >= x && tx < x + btnW && ty >= y && ty < y + SET_BTN_H) {
      if (def.destructive) {
        bool armed = (confirmArmedRow == settingsLeafIndex &&
                      now - confirmArmedMs < CONFIRM_ARM_MS);
        if (armed) {
          confirmArmedRow = -1;
          def.apply(def.values[i]);  // may never return (e.g. ESP.restart())
        } else {
          confirmArmedRow = settingsLeafIndex;
          confirmArmedMs = now;
        }
      } else {
        def.apply(def.values[i]);
      }
      renderSettings();
      break;
    }
  }
  return true;
}

// ── SET_LIST drag-to-scroll ────────────────────────────────
// The touch controller only reports position while pressed (no velocity/
// gesture primitives), so a tap and a scroll look identical until the finger
// has actually moved: Begin just remembers where the touch started, Move
// live-shifts settingsScrollOffset by the frame-to-frame delta, and End
// checks the accumulated movement to decide whether this was a scroll (do
// nothing more) or a tap (hit-test against the row rects at their current
// scrolled position). Using the last position seen by Move/Begin -- rather
// than re-reading the touch controller at release -- avoids relying on
// whatever getTouch() returns the instant the finger lifts.
static int32_t dragLastX = 0, dragLastY = 0;
static int32_t dragTotalMoveY = 0;  // cumulative |dy| this gesture, px
static const int32_t DRAG_TAP_PX = 8;  // below this total movement, treat release as a tap
// dragActive itself is declared above handleSettingsTouch(), which also needs
// it. Without it, a touch that transitions INTO SET_LIST by some other path
// (the settings-gear tap, or BACK from a leaf) would fall through to Move/End
// using whatever dragLastX/Y a previous, unrelated gesture left behind -- see
// the callers below for how each transition sets this correctly.

void settingsListDragBegin(int32_t tx, int32_t ty) {
  dragLastX = tx;
  dragLastY = ty;
  dragTotalMoveY = 0;
  dragActive = true;
}

void settingsListDragMove(int32_t tx, int32_t ty) {
  if (!dragActive) return;
  int32_t dy = ty - dragLastY;
  dragLastX = tx;
  dragLastY = ty;
  if (dy == 0) return;
  dragTotalMoveY += (dy < 0) ? -dy : dy;

  int newOffset = settingsScrollOffset - dy;  // drag finger up -> scroll list down (reveal later rows)
  if (newOffset < 0) newOffset = 0;
  if (newOffset > SETTINGS_SCROLL_MAX) newOffset = SETTINGS_SCROLL_MAX;
  if (newOffset != settingsScrollOffset) {
    settingsScrollOffset = newOffset;
    renderSettings();
  }
}

void settingsListDragEnd(bool catMode) {
  if (!dragActive) return;  // this touch never went through Begin -- e.g. the same
                             // press that just transitioned in via BACK; ignore its release
  dragActive = false;
  if (dragTotalMoveY >= DRAG_TAP_PX) return;  // was a scroll, not a tap -- nothing else to do
  int32_t tx = dragLastX, ty = dragLastY;

  if (tx >= SET_BACK_X0 && tx < SET_BACK_X1 && ty >= SET_BACK_Y0 && ty < SET_BACK_Y1) {
    settingsScreen = SET_OFF;
    if (!catMode) render();  // catMode: gifTick() resumes drawing next pass
    return;
  }
  for (int idx = 0; idx < SETTINGS_COUNT; idx++) {
    int y = SETTINGS_VIEWPORT_Y0 + idx * SET_ROW_STEP - settingsScrollOffset;
    if (y + SET_ROW_H < SETTINGS_VIEWPORT_Y0 || y > SETTINGS_VIEWPORT_Y0 + SETTINGS_VIEWPORT_H) continue;
    if (tx >= SET_ROW_X0 && tx < SET_ROW_X0 + SET_ROW_W && ty >= y && ty < y + SET_ROW_H) {
      settingsLeafIndex = idx;
      confirmArmedRow = -1;
      settingsScreen = SET_LEAF;
      renderSettings();
      break;
    }
  }
}
