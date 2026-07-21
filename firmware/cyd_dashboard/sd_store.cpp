// SD-card config load/save, diagnostics log, and the self-hosted BMP splash
// loader. Split out of cyd_dashboard.ino.
#include "state.h"

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
  // wifi_ssid/wifi_password from the card are used only when config.h's
  // compiled WIFI_SSID is empty -- exactly the "never configured, let the AP
  // setup portal's saved result take over" case (see runApSetup()). A real
  // WIFI_SSID baked into config.h always wins, so editing config.h and
  // reflashing -- the normal way to change WiFi -- isn't silently shadowed
  // forever by whatever was once submitted through the portal.
  if (cfgWifiSsid.length() == 0) {
    if (!doc["wifi_ssid"].isNull()) cfgWifiSsid = doc["wifi_ssid"].as<String>();
    if (!doc["wifi_password"].isNull()) cfgWifiPassword = doc["wifi_password"].as<String>();
  }
  if (!doc["server_host"].isNull()) cfgServerHost = doc["server_host"].as<String>();
  if (!doc["server_port"].isNull()) cfgServerPort = doc["server_port"].as<int>();
  if (!doc["brightness"].isNull()) {
    cfgBrightness = constrain(doc["brightness"].as<int>(), 0, 255);
  }
  if (!doc["poll_interval_sec"].isNull()) {
    // Clamp to a sane floor so a typo can't hammer the Mac or the card.
    // Stored as the user preference; applyEffectivePoll() (below) turns it
    // into the live POLL_INTERVAL_MS, which Battery Save may stretch.
    int sec = doc["poll_interval_sec"].as<int>();
    cfgPollIntervalSec = (uint32_t)constrain(sec, 5, 3600);
  }
  if (!doc["screen_rotation"].isNull()) cfgScreenRotation = doc["screen_rotation"].as<int>();
  if (!doc["touch_x_min"].isNull()) cfgTouchXMin = doc["touch_x_min"].as<int>();
  if (!doc["touch_x_max"].isNull()) cfgTouchXMax = doc["touch_x_max"].as<int>();
  if (!doc["touch_y_min"].isNull()) cfgTouchYMin = doc["touch_y_min"].as<int>();
  if (!doc["touch_y_max"].isNull()) cfgTouchYMax = doc["touch_y_max"].as<int>();
  if (!doc["touch_offset_rotation"].isNull()) cfgTouchOffsetRotation = doc["touch_offset_rotation"].as<int>();
  if (!doc["pixel_shift_min"].isNull()) {
    // Minutes per anti-retention orbit step; 0 pins the frame at (0,0).
    int m = constrain(doc["pixel_shift_min"].as<int>(), 0, 60);
    cfgShiftStepMs = (uint32_t)m * 60000;
  }
  if (!doc["boot_page"].isNull()) {
    cfgBootPage = constrain(doc["boot_page"].as<int>(), 0, PAGE_COUNT - 1);
  }
  if (!doc["cat_shuffle_sec"].isNull()) {
    // 0 disables the early cutoff (GIFs always play to their natural end).
    int s = constrain(doc["cat_shuffle_sec"].as<int>(), 0, 300);
    catShuffleMs = (uint32_t)s * 1000;
  }
  if (!doc["night_mode_preset"].isNull()) {
    cfgNightModeOn = doc["night_mode_preset"].as<int>() != 0;
  }
  if (!doc["show_countdown"].isNull()) {
    cfgShowCountdown = doc["show_countdown"].as<int>() != 0;
  }
  if (!doc["battery_save"].isNull()) {
    // 0=OFF, 1=ON, 2=AUTO (legacy 0/1 still map correctly).
    cfgBatterySaveMode = constrain(doc["battery_save"].as<int>(),
                                   BATTERY_SAVE_OFF, BATTERY_SAVE_AUTO);
  }
  // Apply after all related keys are loaded so Battery Save can floor the
  // poll interval against the user's poll_interval_sec preference. AUTO
  // won't floor until a live fetch sets serverBatterySave.
  applyEffectivePoll();
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

// ── CONFIG.JSON READ-MODIFY-WRITE ──────────────────────────
// saveWifiCredsToSD/saveIntConfigToSD/forgetWifiFromSD each used to hand-roll
// the same read -> parse -> mutate -> remove -> rewrite dance. One helper
// takes a plain function pointer (not std::function -- flash is the scarce
// resource on this board) that mutates the in-memory JsonDocument; the I/O
// around it happens exactly once.
static void mutateConfigJson(void (*mutate)(JsonDocument&, const void*), const void* arg) {
  if (!STATE.sdOk) return;
  lockSD();
  JsonDocument doc;
  File in = SD.open("/config.json", FILE_READ);
  if (in) {
    String payload = in.readString();
    in.close();
    deserializeJson(doc, payload);  // malformed/missing -> doc just stays empty
  }
  mutate(doc, arg);

  SD.remove("/config.json");  // FILE_WRITE appends on this SD library -- remove for a clean overwrite
  File out = SD.open("/config.json", FILE_WRITE);
  if (out) {
    serializeJson(doc, out);
    out.close();
  }
  unlockSD();
}

struct WifiCreds { String ssid, password; };

static void mutateSetWifiCreds(JsonDocument& doc, const void* arg) {
  const WifiCreds* creds = static_cast<const WifiCreds*>(arg);
  doc["wifi_ssid"] = creds->ssid;
  doc["wifi_password"] = creds->password;
}

// Called from ap_setup.cpp's runApSetup(). Unlike the other config mutators
// this must succeed even when STATE.sdOk gates everything else here, but
// runApSetup() only runs when STATE.sdOk is already true (see its caller in
// setup()), so mutateConfigJson's guard is never the reason this silently
// no-ops in practice.
void saveWifiCredsToSD(const String& ssid, const String& password) {
  WifiCreds creds{ssid, password};
  mutateConfigJson(mutateSetWifiCreds, &creds);
}

struct IntConfigKV { const char* key; int32_t value; };

static void mutateSetIntConfig(JsonDocument& doc, const void* arg) {
  const IntConfigKV* kv = static_cast<const IntConfigKV*>(arg);
  doc[kv->key] = kv->value;
}

// Persists any single-int Settings value to /config.json. Called from
// networkTask (core 0) only -- see pendingConfigSave in cyd_dashboard.ino's
// networkTask() -- so this never contends with the render core for the SD
// bus outside of sdMutex. One generic function serves every setting whose
// live value is a plain int, rather than a near-duplicate save function per
// setting.
void saveIntConfigToSD(const char* key, int32_t value) {
  IntConfigKV kv{key, value};
  mutateConfigJson(mutateSetIntConfig, &kv);
}

static void mutateForgetWifi(JsonDocument& doc, const void* /*arg*/) {
  doc.remove("wifi_ssid");
  doc.remove("wifi_password");
}

// Erases the saved WiFi credentials from /config.json (Forget WiFi), leaving
// every other key untouched. Only meaningful when config.h's compiled
// WIFI_SSID is blank -- if it's baked in, cfgWifiSsid will still be non-empty
// at the next boot regardless of this, and runApSetup() won't trigger (same
// precedent as loadRuntimeConfig(): a compiled WIFI_SSID always wins). Called
// from networkTask (core 0) only, via pendingForgetWifi.
void forgetWifiFromSD() {
  mutateConfigJson(mutateForgetWifi, nullptr);
}
