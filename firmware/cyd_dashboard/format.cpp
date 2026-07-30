// Formatting helpers + live resource-percent readers. Split out of
// cyd_dashboard.ino (see state.h for the shared declarations these need).
#include "state.h"

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

// Day-aware countdown for windows that can span multiple days (the weekly
// reset). >=1 day: "1d:12h" (days + hours only). <1 day: "23h:59m", same
// shape as fmtCountdown. Twin of simulator.html's fmtCountdownDHM.
String fmtCountdownDHM(long sec) {
  if (sec < 0) return "";
  long totalMin = (sec + 30) / 60;  // round to nearest minute
  long days = totalMin / 1440;
  char buf[16];
  if (days > 0) {
    long hours = (totalMin % 1440) / 60;
    snprintf(buf, sizeof(buf), "%ldd:%ldh", days, hours);
  } else {
    long hours = totalMin / 60;
    long mins = totalMin % 60;
    snprintf(buf, sizeof(buf), "%ldh:%02ldm", hours, mins);
  }
  return String(buf);
}

String fmtKB(uint32_t bytes) {
  return String((float)bytes / 1024.0f, 1) + "KB";
}

String fmtGB(uint64_t bytes) {
  return String((float)bytes / (1024.0f * 1024.0f * 1024.0f), 1) + "GB";
}

// Flash used by the sketch as a % of the OTA app partition. ESP.getSketchSize()
// walks the flash image and is measurably slow on this core, and the sketch
// size cannot change at runtime — computed once at boot (see cyd_dashboard.ino's
// setup()) and cached, rather than re-queried on every 1s footer/device-page
// render while stateMutex is held.
uint32_t cachedFlashUsed = 0, cachedFlashTotal = 0;
bool flashCacheValid = false;

int flashPercent(uint32_t &usedOut, uint32_t &totalOut) {
  if (!flashCacheValid) {
    cachedFlashUsed = ESP.getSketchSize();
    cachedFlashTotal = cachedFlashUsed + ESP.getFreeSketchSpace();
    flashCacheValid = true;
  }
  usedOut = cachedFlashUsed;
  totalOut = cachedFlashTotal;
  return cachedFlashTotal ? (int)((float)cachedFlashUsed / cachedFlashTotal * 100 + 0.5f) : -1;
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

// SD card space in use -- cheap FAT bookkeeping reads, not a scan of the
// card, but SD.totalBytes()/usedBytes() still touch the HSPI bus, so this raw
// query must only run under lockSD()/unlockSD() from networkTask (core 0),
// never from the render core. -1 (with usedOut/totalOut untouched) when the
// card never mounted.
static int sdCapacityPercentRaw(uint64_t &usedOut, uint64_t &totalOut) {
  if (!STATE.sdOk) return -1;
  uint64_t total = SD.totalBytes();
  uint64_t used = SD.usedBytes();
  usedOut = used;
  totalOut = total;
  return total ? (int)((float)used / total * 100 + 0.5f) : -1;
}

// cachedSd* are plain (non-atomic) globals: refreshSdCapacityCache() (core 0,
// under sdMutex) is the only writer, drawDevicePage() (core 1) the only
// reader, and a torn read of a multi-byte value would at worst show one
// stale/mixed frame of a cosmetic stat -- self-correcting next refresh, and
// far cheaper than a lock render() can't take (see state.h's note on why
// stateMutex -> sdMutex nesting is forbidden).
static int cachedSdPercent = -1;
static uint64_t cachedSdUsedBytes = 0;
static uint64_t cachedSdTotalBytes = 0;

void refreshSdCapacityCache() {
  uint64_t used = 0, total = 0;
  lockSD();
  int pct = sdCapacityPercentRaw(used, total);
  unlockSD();
  cachedSdPercent = pct;
  cachedSdUsedBytes = used;
  cachedSdTotalBytes = total;
}

int cachedSdCapacityPercent(uint64_t &usedOut, uint64_t &totalOut) {
  usedOut = cachedSdUsedBytes;
  totalOut = cachedSdTotalBytes;
  return cachedSdPercent;
}
