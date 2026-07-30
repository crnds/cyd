// Frame presentation (pixel-shift, dirty-band pushes) + every full-screen
// page's draw function. This is the near-line-for-line twin of
// simulator.html's page-drawing functions — see the parent CLAUDE.md's
// firmware/simulator parity rule before touching layout, colors, or text
// here without updating the simulator too. Split out of cyd_dashboard.ino.
#include "state.h"

// Shared by status-page digital date and weather overlay daily rows.
static const char* const WDAY_ABBR[7] = {
  "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};

// Keep a quiet gutter at the physical panel's right edge on every composed
// frame. Applying it at presentation time makes the inset uniform across all
// dashboard pages, settings, overlays, and offline/cat states.
static const int SCREEN_RIGHT_PADDING = 4;
static const int FOOTER_RIGHT_PADDING = 8;

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

// Advance the pixel-shift orbit (core 1 only, like everything display-side).
// Returns true on a step so the caller can repaint immediately — the panel
// never straddles two offsets (shiftDirty forces the next presentFrame to be
// a full blit).
bool pixelShiftTick(uint32_t now) {
  if (cfgShiftStepMs == 0 || g != &frame) return false;
  if (now - lastShiftMs < cfgShiftStepMs) return false;
  lastShiftMs = now;
  shiftIdx = (uint8_t)((shiftIdx + 1) % 8);
  shiftX = SHIFT_ORBIT[shiftIdx][0];
  shiftY = SHIFT_ORBIT[shiftIdx][1];
  shiftDirty = true;
  return true;
}

// Blank the panel strips a shifted blit leaves uncovered (left shiftX
// columns, bottom -shiftY rows) so no stale pixels linger at the edges.
void clearShiftMargins() {
  if (shiftX > 0) gfx.fillRect(0, 0, shiftX, 240, 0x0000);
  if (shiftY < 0) gfx.fillRect(0, 240 + shiftY, 320, -shiftY, 0x0000);
}

bool checkHourlyFlash(bool& isEvenSecond) {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 0)) {
    if (timeinfo.tm_min == 0 && timeinfo.tm_sec < 6) {
      isEvenSecond = (timeinfo.tm_sec % 2 == 0);
      return true;
    }
  }
  return false;
}

// Push one w×h region of the frame sprite to the panel in a single SPI
// transaction. LGFXBase::pushImage() assumes a tightly-packed source (it
// overwrites pixelcopy_t::src_bitwidth with w), so this calls the panel's
// writeImage() directly: its strided-source path (src_bitwidth = the frame's
// 320px row pitch) sets the window once and then streams every row — one DMA
// transfer for the 16bpp sprite, per-line palette conversion into the bus's
// DMA buffer for the 8bpp fallback — replacing the old per-row pushImage loop
// that cost hundreds of SPI transactions per frame on the mixed page.
// Clipping happens here because the panel-level call skips LGFXBase's own
// clip step (the pixel-shift offset can push a region's edges past the panel).
static void pushFrameRegion(int x, int y, int w, int h) {
  int dx = x + shiftX, dy = y + shiftY;
  int sx = 0, sy = 0;
  if (dx < 0) { sx = -dx; w -= sx; dx = 0; }
  if (dy < 0) { sy = -dy; h -= sy; dy = 0; }
  if (dx + w > 320) w = 320 - dx;
  if (dy + h > 240) h = 240 - dy;
  if (w <= 0 || h <= 0) return;
  void* buf = frame.getBuffer();
  size_t offset = (size_t)(y + sy) * 320 + (x + sx);
  lgfx::pixelcopy_t pc;
  bool useDma = false;
  if (frame.getColorDepth() == 16) {
    pc = lgfx::pixelcopy_t((const uint16_t*)buf + offset, lgfx::rgb565_2Byte, lgfx::rgb565_2Byte);
    useDma = esp_ptr_dma_capable(buf);
  } else {
    // 8bpp palette sprite: panel-side conversion via the sprite's palette,
    // mirroring how LGFX_Sprite::pushSprite() builds its pixelcopy.
    pc = lgfx::pixelcopy_t((const uint8_t*)buf + offset, gfx.getColorDepth(),
                           (lgfx::color_depth_t)frame.getColorDepth(),
                           gfx.hasPalette(), frame.getPalette());
  }
  pc.src_bitwidth = 320;
  pc.src_width = w;
  pc.src_height = h;
  gfx.startWrite();
  gfx.getPanel()->writeImage(dx, dy, w, h, &pc, useDma);
  gfx.endWrite();
}

void presentFrame(bool fullScreen) {
  bool isEvenSecond = false;
  if (checkHourlyFlash(isEvenSecond)) {
    gfx.invertDisplay(isEvenSecond);
  } else {
    gfx.invertDisplay(false);
  }

  if (g == &frame) {
    bool offline = !STATE.haveData;
    // All destination coords carry the pixel-shift offset; pushFrameRegion
    // clips against the panel itself. After an orbit step (shiftDirty) the
    // partial path would leave half the screen at the old offset, so that one
    // frame falls through to the full blit below.
    if (currentPage == MIXED_PAGE && !offline && !shiftDirty) {
      if (fullScreen) {
        // Push left half (0-159, height 240) + right footer (160-319, rows 220-239)
        pushFrameRegion(0, 0, 160, 240);
        pushFrameRegion(160, 220, 160, 20);
      } else if (gifMinY <= gifMaxY) {
        // Push only the dirty band of the right half (160-319) where the GIF is drawn
        int startY = gifMinY < 0 ? 0 : gifMinY;
        int endY = gifMaxY >= 220 ? 219 : gifMaxY;
        if (endY >= startY) pushFrameRegion(160, startY, 160, endY - startY + 1);
      }
    } else {
      frame.pushSprite(shiftX, shiftY);
      clearShiftMargins();
      shiftDirty = false;
    }

    // The framebuffer remains 320px wide for touch and layout parity; mask the
    // final four panel pixels only after the frame has been presented.
    gfx.fillRect(320 - SCREEN_RIGHT_PADDING, 0, SCREEN_RIGHT_PADDING, 240, COL_BG);
  }
}

// Small 3-bar signal icon next to the pulse dot: green when the most recent
// poll found WiFi up (wifiOk), rose when it didn't. Separate from the pulse
// dot below so a WiFi outage and a server outage (WiFi fine, Mac unreachable)
// read as two distinct signals instead of collapsing into one amber dot.
static void drawWifiIcon() {
  uint16_t c = wifiOk ? COL_GOOD : COL_WARN;
  g->fillRect(20, 230, 2, 3, c);
  g->fillRect(23, 228, 2, 5, c);
  g->fillRect(26, 226, 2, 7, c);
}

// Footer settings gear, bottom-right corner (SETTINGS_HIT_* in state.h is
// its tap target). A hub disc with a punched-out center + 8 short spokes --
// same circle/drawWideLine vocabulary as drawWeatherIcon's sun glyph, just
// smaller and gray so it reads as UI chrome rather than a page icon.
static void drawSettingsIcon() {
  const int cx = 299, cy = 230;
  g->fillCircle(cx, cy, 3, COL_TEXT2);
  g->fillCircle(cx, cy, 1, COL_BG);
  g->drawWideLine(cx, cy - 6, cx, cy - 4, 1.0f, COL_TEXT2);
  g->drawWideLine(cx, cy + 4, cx, cy + 6, 1.0f, COL_TEXT2);
  g->drawWideLine(cx - 6, cy, cx - 4, cy, 1.0f, COL_TEXT2);
  g->drawWideLine(cx + 4, cy, cx + 6, cy, 1.0f, COL_TEXT2);
  g->drawWideLine(cx - 4, cy - 4, cx - 3, cy - 3, 1.0f, COL_TEXT2);
  g->drawWideLine(cx + 3, cy + 3, cx + 4, cy + 4, 1.0f, COL_TEXT2);
  g->drawWideLine(cx - 4, cy + 4, cx - 3, cy + 3, 1.0f, COL_TEXT2);
  g->drawWideLine(cx + 3, cy - 3, cx + 4, cy - 4, 1.0f, COL_TEXT2);
}

// ── DRAWING HELPERS ────────────────────────────────────────
void drawFooter() {
  // Status dot: server reachability only, gated on wifiOk so it stays blank
  // (no color) whenever WiFi itself is down — that case is already covered
  // by drawWifiIcon() above, so lighting the dot too would misread as a
  // server-side problem. When WiFi is up: blinks once a second while the
  // most recent poll reached the server — green on the odd second, off on
  // the even second, so the pulse is easy to read at a glance. Static amber
  // when WiFi is fine but the poll still didn't reach the server (coasting
  // on cached data).
  bool onPhase = (millis() / 1000) % 2;
  if (wifiOk) {
    if (!connected) g->fillCircle(14, 230, 3, COL_ACCENT);
    else if (onPhase) g->fillCircle(14, 230, 4, COL_GOOD);
  }
  drawWifiIcon();
  drawSettingsIcon();

  String pageStr = String(currentPage + 1) + " / " + String(PAGE_COUNT);
  g->setTextColor(COL_TEXT2);
  g->setTextSize(1);
  // Right-aligned just left of the settings gear (SETTINGS_HIT_X0), not the
  // panel edge, so the two never overlap.
  g->setCursor(SETTINGS_HIT_X0 - FOOTER_RIGHT_PADDING - (int)pageStr.length() * 6, 226);
  g->print(pageStr);

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
  int lineW = (int)((float)elapsed / POLL_INTERVAL_MS * 320);
  if (lineW > 0) g->fillRect(0, 239, lineW, 1, COL_TEXT2);
}

// Small top-right corner overlay shown on every page whenever Battery Save is
// *active* (Settings ON, or AUTO + Mac power.battery_save) -- a persistent
// reminder that the usage-poll interval is floored at BATTERY_SAVE_POLL_SEC.
// No-ops when inactive, so callers can invoke it unconditionally. A solid
// backing box behind the icon mirrors drawSessionResetOverlay()'s legibility
// treatment over cat GIF frames. BATTERY_ICON_Y0/Y1 (state.h) mirror this
// box's y-range for gif_player.cpp's dirty-band fold-in.
void drawBatterySaveIcon() {
  if (!batterySaveActive()) return;
  const int boxX = 296, boxW = 22, boxH = BATTERY_ICON_Y1 - BATTERY_ICON_Y0 + 1;
  g->fillRect(boxX, BATTERY_ICON_Y0, boxW, boxH, COL_BG);
  const int bodyX = 300, bodyY = 5, bodyW = 13, bodyH = 7;
  const int nubW = 2, nubH = 3;
  g->fillRect(bodyX, bodyY, bodyW, bodyH, COL_YELLOW);
  g->fillRect(bodyX + bodyW, bodyY + (bodyH - nubH) / 2, nubW, nubH, COL_YELLOW);
}

// Percent track+fill. minFillPx is the smallest non-zero fill (3 for thick
// bars so a 1% reading is still visible; h for thin mini-bars so a full-height
// stub shows). Returns fill width or -1 when percent is unknown.
static int drawPercentBar(int x, int y, int w, int h, int percent,
                          uint16_t color, uint16_t trackColor = COL_TRACK,
                          int minFillPx = 3) {
  g->fillRect(x, y, w, h, trackColor);
  if (percent < 0) return -1;
  int fillW = (int)((float)min(percent, 100) / 100 * w + 0.5f);
  if (fillW < minFillPx) fillW = minFillPx;
  g->fillRect(x, y, fillW, h, color);
  return fillW;
}

// Live countdown: tick the last server-provided remaining-seconds down by
// wall time since lastFetchOkMs. Shared by Home / status / limits card.
static long liveResetsInSec(long baseSec) {
  if (baseSec < 0) return -1;
  long rem = baseSec - (long)((millis() - STATE.lastFetchOkMs) / 1000);
  return rem < 0 ? 0 : rem;
}

static void drawLimitBlock(int y, const char* title, int percent, const char* resets, long resetsInSec) {
  g->setTextColor(COL_TEXT);
  g->setTextSize(1);
  g->setCursor(10, y);
  g->print(title);

  int barX = 10, barY = y + 22, barW = 191, barH = 16;
  drawPercentBar(barX, barY, barW, barH, percent, COL_ACCENT);

  g->setTextColor(COL_TEXT);
  g->setTextSize(2);
  g->setCursor(211, barY + 1);
  g->print(percent >= 0 ? String(percent) + "% used" : "--");

  g->setTextColor(COL_TEXT2);
  g->setTextSize(2);
  g->setCursor(10, barY + 26);
  bool haveResets = resets[0] != '\0';
  String line = haveResets ? String("Resets ") + resets : String("Resets --");
  if (haveResets && resetsInSec >= 0) {
    line += " (" + fmtCountdown(resetsInSec) + ")";
  }
  g->print(line);
}
// One row of the /usage-style limits page: label left, right-aligned value,
// thin 8px bar under. percent < 0 leaves the track empty (unknown).
static void drawLimitsRow(int y, const String& label, const String& right, int percent) {
  g->setTextSize(1);
  g->setTextColor(COL_TEXT);
  g->setCursor(10, y);
  g->print(label);
  g->setTextColor(COL_TEXT2);
  g->setCursor(320 - (int)right.length() * 6, y);
  g->print(right);
  drawPercentBar(10, y + 12, 310, 8, percent, COL_ACCENT);
}

// "Resets Jul 16, 04:59  58%" — or bare "58%", or "--" when unknown.
static String limitRowText(int percent, const char* resets) {
  if (percent < 0) return "--";
  String s;
  if (resets[0] != '\0') s = String("Resets ") + resets + "  ";
  s += String(percent) + "%";
  return s;
}

// Limits page (index 4): Claude Code /usage panel — context window, 5-hour
// limit, weekly (all models), weekly per-model (hidden when the server sends
// null), usage credits. Rows shift up when a row is absent.
static void drawLimitsPage() {
  g->setTextColor(COL_TEXT);
  g->setTextSize(1);
  g->setCursor(10, 6);
  g->print("USAGE LIMITS");

  int y = 22;
  String ctx = STATE.ctxTokens >= 0
      ? fmtTokens(STATE.ctxTokens) + "  " + String(STATE.ctxPercent) + "%"
      : String("--");
  drawLimitsRow(y, "Context window", ctx, STATE.ctxPercent); y += 40;

  drawLimitsRow(y, "5-hour limit",
                limitRowText(STATE.sessionPercent, STATE.sessionResets),
                STATE.sessionPercent); y += 40;

  drawLimitsRow(y, "Weekly (all models)",
                limitRowText(STATE.weekPercent, STATE.weekResets),
                STATE.weekPercent); y += 40;

  if (STATE.weekModelPercent >= 0) {
    String name = STATE.weekModelName[0] != '\0' ? String(STATE.weekModelName) : String("model");
    drawLimitsRow(y, "Weekly (" + name + ")",
                  limitRowText(STATE.weekModelPercent, STATE.weekModelResets),
                  STATE.weekModelPercent); y += 40;
  }

  if (STATE.creditsUsed >= 0) {
    drawLimitsRow(y, "Usage credits",
                  fmtCost(STATE.creditsUsed) + " of " + fmtCost(STATE.creditsLimit),
                  STATE.creditsPercent);
  }
}

static void drawHomePage() {
  long sessionRem = liveResetsInSec(STATE.sessionResetsInSec);
  // Week has no countdown suffix here: its "Resets <date>" string is already
  // close to the line's pixel budget (see drawLimitsCard's size1 note below),
  // so appending " (HHh:MMm)" would overflow the 320px screen.
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
static void drawProjectsPage() {
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
    int barMaxW = 246;
    for (int i = 0; i < shown; i++) {
      g->setTextColor(COL_TEXT);
      g->setTextSize(1);
      g->setCursor(10, y);
      g->print(STATE.projectNames[i]);

      int barW = (int)((float)STATE.projectTokens[i] / maxTokens * barMaxW);
      g->fillRect(10, y + 10, barMaxW, 8, COL_SURFACE);
      g->fillRect(10, y + 10, max(barW, 4), 8, COL_ACCENT);

      g->setTextColor(COL_TEXT2);
      g->setCursor(10 + barMaxW + 8, y + 11);
      g->print(fmtTokens(STATE.projectTokens[i]));

      y += 26;
    }
  }

  // ── lower half: 7-day trend ──
  g->fillRect(10, 128, 310, 1, COL_BORDER);
  g->setTextColor(COL_TEXT);
  g->setTextSize(1);
  g->setCursor(10, 134);
  g->print("7-DAY TREND");

  int64_t maxTrend = 1;
  for (int i = 0; i < 7; i++) {
    if (STATE.trend[i] > maxTrend) maxTrend = STATE.trend[i];
  }

  const char* dayLabels[7] = {"-6", "-5", "-4", "-3", "-2", "-1", "today"};
  int chartX = 24, chartY = 150, chartH = 64, barW = 34, gap = 8;
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
// from circles/rects/lines, mapped from Open-Meteo's WMO weather_code. cx,cy
// is the center of a ~24x20px icon anchored at (x,y).
// Scaled down (~0.65x) from the original so the weather card can shrink to
// the clock card's short bottom-row height (see drawStatusPage).
static void drawWeatherIcon(int x, int y, int code) {
  if (code < 0) {
    g->setTextColor(COL_TEXT2);
    g->setTextSize(1);
    g->setCursor(x, y + 4);
    g->print("--");
    return;
  }
  int cx = x + 8, cy = y + 7;
  if (code == 0 || code == 1) {
    // clear: sun disc + 8 short rounded rays (cardinals then diagonals),
    // with a 2-3px gap between disc and rays so they read as separate.
    g->fillCircle(cx, cy, 4, COL_YELLOW);
    g->drawWideLine(cx, cy - 9, cx, cy - 7, 1.0f, COL_YELLOW);
    g->drawWideLine(cx, cy + 7, cx, cy + 9, 1.0f, COL_YELLOW);
    g->drawWideLine(cx - 9, cy, cx - 7, cy, 1.0f, COL_YELLOW);
    g->drawWideLine(cx + 7, cy, cx + 9, cy, 1.0f, COL_YELLOW);
    g->drawWideLine(cx - 7, cy - 7, cx - 5, cy - 5, 1.0f, COL_YELLOW);
    g->drawWideLine(cx + 5, cy + 5, cx + 7, cy + 7, 1.0f, COL_YELLOW);
    g->drawWideLine(cx - 7, cy + 7, cx - 5, cy + 5, 1.0f, COL_YELLOW);
    g->drawWideLine(cx + 5, cy - 5, cx + 7, cy - 7, 1.0f, COL_YELLOW);
    return;
  }
  // Rain/snow/lightning are drawn standalone (no cloud underneath) so the
  // condition itself reads clearly at this icon's small size — a cloud
  // sharing the space with a tiny overlay was too cluttered to tell apart.
  if (code >= 95) {
    // thunderstorm: zigzag bolt — a Material-style hexagon polygon
    // (bottom tip, notch, top prong) split into 4 fillTriangle calls
    // since the GFX API has no filled-polygon primitive. Vertices:
    // A(-1,+8) B(-1,+2) C(-5,+2) D(+1,-8) E(+1,-2) F(+5,-2).
    g->fillTriangle(cx - 5, cy + 2, cx + 1, cy - 8, cx + 1, cy - 2, COL_YELLOW);
    g->fillTriangle(cx - 5, cy + 2, cx + 1, cy - 2, cx - 1, cy + 2, COL_YELLOW);
    g->fillTriangle(cx - 1, cy + 2, cx + 1, cy - 2, cx + 5, cy - 2, COL_YELLOW);
    g->fillTriangle(cx - 1, cy + 2, cx + 5, cy - 2, cx - 1, cy + 8, COL_YELLOW);
    return;
  }
  if ((code >= 71 && code <= 77) || code == 85 || code == 86) {
    // snow: six-armed snowflake = three rounded lines crossing at 60deg
    g->drawWideLine(cx, cy - 7, cx, cy + 7, 1.0f, COL_TEXT);
    g->drawWideLine(cx - 6, cy - 4, cx + 6, cy + 4, 1.0f, COL_TEXT);
    g->drawWideLine(cx - 6, cy + 4, cx + 6, cy - 4, 1.0f, COL_TEXT);
    return;
  }
  if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) {
    // rain: three staggered teardrops (triangle cap fused onto a circle),
    // two small on top, one large below — classic rain glyph.
    g->fillTriangle(cx - 6, cy - 8, cx - 8, cy - 3, cx - 4, cy - 3, COL_BLUE);
    g->fillCircle(cx - 6, cy - 3, 2, COL_BLUE);
    g->fillTriangle(cx + 5, cy - 6, cx + 3, cy - 1, cx + 7, cy - 1, COL_BLUE);
    g->fillCircle(cx + 5, cy - 1, 2, COL_BLUE);
    g->fillTriangle(cx - 1, cy + 1, cx - 4, cy + 6, cx + 2, cy + 6, COL_BLUE);
    g->fillCircle(cx - 1, cy + 6, 3, COL_BLUE);
    return;
  }
  // Everything else shares a plain cloud (2/3/45/48 = cloudy/fog, or any
  // unmapped code): two overlapping puffs on a fully-rounded pill base.
  g->fillCircle(cx - 4, cy - 2, 4, COL_TEXT2);
  g->fillCircle(cx + 3, cy - 3, 5, COL_TEXT2);
  g->fillRoundRect(cx - 9, cy - 2, 19, 9, 4, COL_TEXT2);
}

// Thin inset progress bar (min fill = bar height so a stub is full-height).
// Returns fill width (-1 when unknown) for shine-sweep overlays.
static int drawMiniBar(int x, int y, int w, int percent, uint16_t color,
                       uint16_t trackColor = COL_TRACK, int h = 6) {
  return drawPercentBar(x, y, w, h, percent, color, trackColor, h);
}

// ── SHINE SWEEP ────────────────────────────────────────────
// Looping light band swept across the green reset-countdown bars' fill in
// drawLimitsCard (status page 0 + MIXED_PAGE). The band crosses the full
// track at constant speed but is only painted over the filled interior.
// Twin of simulator.html's drawShineStrip.
static const uint32_t SHINE_PERIOD_MS = 2600;
static const int SHINE_BAND_R = 7;
static const int SHINE_BAR_X = 12, SHINE_BAR_W = 137, SHINE_BAR_H = 4;
static const int SHINE_BAR_Y[2] = {51, 150};  // 5h, weekly (drawLimitsCard)
// Fill widths cached by drawLimitsCard (under stateMutex) so shineTick can
// repaint between renders without touching STATE — same lock-free-volatile
// pattern as loop()'s poll-progress line.
static volatile int shineFillPx[2] = {-1, -1};

// Paint the shine band over one bar's filled interior on `dst` — the sprite
// during drawLimitsCard (so pushed frames already carry the band), or the
// panel directly from shineTick (caller adds the pixel-shift offset).
// Interior columns only (2..fillW-3, full height) so the radius-2 rounded
// ends stay untouched; repainting non-band columns COL_GOOD erases the trail.
// Only columns whose color actually changed since the previous call for the
// same bar are written: the band moves ~2px per 30ms shineTick pass, so a
// full repaint was ~130 tiny panel writes per strip per pass where only a
// dozen columns change — the dominant constant CPU cost on the shine pages.
static int shinePrevCenter[2] = {INT_MIN, INT_MIN};

static inline uint16_t shineColor(int i, int center) {
  int d = abs(i - center);
  return d <= 1 ? COL_SHINE_HI : d <= 4 ? COL_SHINE_MID : d <= SHINE_BAND_R ? COL_SHINE_LO : COL_GOOD;
}

static void drawShineStrip(lgfx::LovyanGFX* dst, int x, int y, int fillW, uint32_t nowMs, int barIdx) {
  if (fillW < 12) return;
  float phase = (float)(nowMs % SHINE_PERIOD_MS) / SHINE_PERIOD_MS;
  int center = -SHINE_BAND_R + (int)(phase * (SHINE_BAR_W + 2 * SHINE_BAND_R));
  int prev = shinePrevCenter[barIdx];
  shinePrevCenter[barIdx] = center;
  dst->startWrite();
  for (int i = 2; i <= fillW - 3; i++) {
    uint16_t c = shineColor(i, center);
    if (prev != INT_MIN && c == shineColor(i, prev)) continue;
    dst->fillRect(x + i, y, 1, SHINE_BAR_H, c);
  }
  dst->endWrite();
}

// ~30ms between-render top-up of the shine band, straight to the panel —
// same pattern as loop()'s poll-progress line: no lock (only the volatile
// cached fill widths), pixel-shift offset applied here, and the inverted
// hourly-flash frames skipped.
void shineTick(uint32_t nowMs) {
  if (currentPage != 0 && currentPage != MIXED_PAGE) return;
  bool isEvenSecond = false;
  if (checkHourlyFlash(isEvenSecond) && isEvenSecond) return;
  for (int i = 0; i < 2; i++) {
    drawShineStrip(&gfx, SHINE_BAR_X + shiftX, SHINE_BAR_Y[i] + shiftY, shineFillPx[i], nowMs, i);
  }
}

// Uppercase section label.
static void drawCardLabel(int x, int y, const char* label) {
  g->setTextColor(COL_TEXT2);
  g->setTextSize(1);
  g->setCursor(x, y);
  g->print(label);
}

static const long SESSION_WINDOW_SEC = 5L * 3600;       // 5h
static const long WEEK_WINDOW_SEC = 7L * 24 * 3600;     // 168h

// Reset-countdown fill: 0% right after a reset, 100% right before the next
// one. `remainingSec` is the live countdown (already ticked down locally
// between polls); `windowSec` is the fixed window length (5h or 168h).
static int elapsedPercentOfWindow(long remainingSec, long windowSec) {
  if (remainingSec < 0) return -1;
  long elapsed = windowSec - remainingSec;
  if (elapsed < 0) elapsed = 0;
  int pct = (int)(elapsed * 100 / windowSec);
  return constrain(pct, 0, 100);
}

static void drawLimitsCard() {
  // Two separate cards (5h / week) with a 2px gap between them, rather than
  // one tall card split by an internal divider line (all card-to-card gaps
  // on this page are 2px — see drawStatusPage's comment). Card height grew
  // by the space freed from shrinking the old 4-6px gaps down to 2px, giving
  // the content a bit more breathing room instead of just bottom padding.
  g->fillRect(2, 2, 157, 99, COL_SURFACE);
  g->drawRect(2, 2, 157, 99, COL_BORDER);
  g->fillRect(2, 103, 157, 87, COL_SURFACE);
  g->drawRect(2, 103, 157, 87, COL_BORDER);

  long sessionRem = liveResetsInSec(STATE.sessionResetsInSec);
  long weekRem = liveResetsInSec(STATE.weekResetsInSec);

  // ── left card: limits ──
  String sessionPctStr = STATE.sessionPercent >= 0 ? String(STATE.sessionPercent) + "%" : "--";
  g->setTextColor(COL_ACCENT);
  g->setTextSize(3);
  g->setCursor(12, 11);
  g->print(sessionPctStr);
  drawCardLabel(12 + sessionPctStr.length() * 18 + 6, 27, "5H");

  drawMiniBar(12, 41, 137, STATE.sessionPercent, COL_ACCENT);
  // Green reset-countdown bars (and their shine) are optional via Settings
  // "Show Countdown"; when off, clear the cached fill so shineTick no-ops.
  if (cfgShowCountdown) {
    shineFillPx[0] = drawMiniBar(12, 51, 137, elapsedPercentOfWindow(sessionRem, SESSION_WINDOW_SEC), COL_GOOD, COL_TRACK_BLACK, 4);
    drawShineStrip(g, SHINE_BAR_X, SHINE_BAR_Y[0], shineFillPx[0], millis(), 0);
  } else {
    shineFillPx[0] = -1;
  }

  g->setTextColor(COL_TEXT2);
  g->setTextSize(2);
  g->setCursor(12, 59);
  g->print(STATE.sessionResets[0] != '\0' ? String("resets ") + STATE.sessionResets : String("resets --"));
  g->setTextColor(COL_TEXT);
  g->setTextSize(2);
  g->setCursor(12, 77);
  if (sessionRem >= 0) g->print("in " + fmtCountdown(sessionRem));

  String weekPctStr = STATE.weekPercent >= 0 ? String(STATE.weekPercent) + "%" : "--";
  g->setTextColor(COL_ACCENT);
  g->setTextSize(3);
  g->setCursor(12, 110);
  g->print(weekPctStr);
  drawCardLabel(12 + weekPctStr.length() * 18 + 6, 126, "WEEK");

  drawMiniBar(12, 140, 137, STATE.weekPercent, COL_ACCENT);
  if (cfgShowCountdown) {
    shineFillPx[1] = drawMiniBar(12, 150, 137, elapsedPercentOfWindow(weekRem, WEEK_WINDOW_SEC), COL_GOOD, COL_TRACK_BLACK, 4);
    drawShineStrip(g, SHINE_BAR_X, SHINE_BAR_Y[1], shineFillPx[1], millis(), 1);
  } else {
    shineFillPx[1] = -1;
  }

  // "resets Jul 9, 4:59am" is 20 chars = 120px at size1 — comfortably under the
  // card's inner width, so this line must stay size1.
  g->setTextColor(COL_TEXT2);
  g->setTextSize(1);
  g->setCursor(12, 164);
  g->print(STATE.weekResets[0] != '\0' ? String("resets ") + STATE.weekResets : String("resets --"));

  g->setTextColor(COL_TEXT);
  g->setTextSize(1);
  g->setCursor(12, 174);
  if (weekRem >= 0) g->print("in " + fmtCountdownDHM(weekRem));
}

// BTC price card: sits directly under the week card in the left column,
// using the same 2px-gap convention as the rest of the page (see
// drawStatusPage's comment). Shared by drawStatusPage and
// drawMixedPageStatic so both pages show the same in-line "BTCUSDT <price>"
// card.
static void drawBtcCard() {
  g->fillRect(2, 192, 157, 26, COL_SURFACE);
  g->drawRect(2, 192, 157, 26, COL_BORDER);

  // Label + price in-line ("BTCUSDT 65,920") rather than stacked "BTC/USDT" +
  // price, so the whole thing fits in a short card. Label stays size1
  // (size2 wouldn't fit alongside a size2 price in the 157px card width).
  // Vertically centered in the 26px-tall card so both lines get an even pad.
  g->setTextColor(COL_TEXT2);
  g->setTextSize(1);
  g->setCursor(12, 201);
  g->print("BTCUSDT");

  g->setTextColor(COL_TEXT);
  g->setTextSize(2);
  g->setCursor(60, 197);
  g->print(fmtBtc(STATE.btcPrice));
}

void drawMixedPageStatic() {
  // Clear the footer background area to prevent text bloating/overlapping
  g->fillRect(0, 220, 320, 20, COL_BG);
  drawLimitsCard();
  drawBtcCard();
  drawFooter();
}

// Timer region fill color: COL_GOOD blended 50% into COL_BG/COL_SURFACE
// (the clock card's background — see drawTimerWedge below), precomputed in
// RGB565 5-6-5 channel space since this display has no real alpha channel.
// COL_BG=0x0841 (R5=1,G6=2,B5=1) + COL_GOOD=0x2668 (R5=4,G6=51,B5=8),
// averaged and rounded per-channel -> R5=3,G6=27,B5=5 -> 0x1B65 (~rgb(25,109,41)).
const uint16_t COL_GOOD_50 = 0x1B65;

// Filled pie wedge from the hour hand's current angle clockwise to the
// reset angle — a "time remaining" region layered under the ticks/hands so
// they stay legible on top. Twin of simulator.html's drawTimerWedge, but
// using LovyanGFX's fillArc (degrees, no radian conversion needed) plus the
// precomputed solid blend color above instead of true alpha.
static void drawTimerWedge(int cx, int cy, int r, float startAngle, float endAngle) {
  float delta = fmodf(endAngle - startAngle, 360.0f);
  if (delta < 0) delta += 360.0f;
  g->fillArc(cx, cy, r, 0, startAngle, startAngle + delta, COL_GOOD_50);
}

// Minimal analog clock: a bare circle, 12/3/6/9 tick dots, and hour/minute
// hands — no numerals, no second hand. Angles are screen-space (y grows
// down) with a -90° offset so 0 minutes/hours points straight up (12
// o'clock); increasing angle then sweeps clockwise on screen, matching a
// real clock face.
static void drawAnalogClock(int cx, int cy, int r, int hour24, int minute, int second,
                             bool haveReset, int resetHour24, int resetMinute) {
  float hourAngle = ((hour24 % 12) + minute / 60.0f) * 30.0f - 90.0f;
  float minAngle = (minute + second / 60.0f) * 6.0f - 90.0f;
  float secAngle = second * 6.0f - 90.0f;
  float hourRad = hourAngle * PI / 180.0f;
  float minRad = minAngle * PI / 180.0f;
  float secRad = secAngle * PI / 180.0f;

  // Timer region: hour hand -> next 5h reset, drawn first so the circle,
  // ticks, and hands all render crisply on top of the fill. Gated by
  // Settings "Show Countdown"; the green reset hand below always draws.
  if (haveReset && cfgShowCountdown) {
    float resetAngleWedge = ((resetHour24 % 12) + resetMinute / 60.0f) * 30.0f - 90.0f;
    drawTimerWedge(cx, cy, r - 1, hourAngle, resetAngleWedge);
  }

  g->drawCircle(cx, cy, r, COL_TEXT);
  // 12 short hour ticks radiating in from the rim, same -90° zero-at-top
  // convention as the hands below.
  for (int i = 0; i < 12; i++) {
    float tickRad = (i * 30.0f - 90.0f) * PI / 180.0f;
    int x0 = cx + (int)(cosf(tickRad) * (r - 1));
    int y0 = cy + (int)(sinf(tickRad) * (r - 1));
    int x1 = cx + (int)(cosf(tickRad) * (r - 5));
    int y1 = cy + (int)(sinf(tickRad) * (r - 5));
    g->drawLine(x0, y0, x1, y1, COL_TEXT);
  }

  int hx = cx + (int)(cosf(hourRad) * r * 0.5f);
  int hy = cy + (int)(sinf(hourRad) * r * 0.5f);
  int mx = cx + (int)(cosf(minRad) * r * 0.8f);
  int my = cy + (int)(sinf(minRad) * r * 0.8f);
  int sx = cx + (int)(cosf(secRad) * r * 0.9f);
  int sy = cy + (int)(sinf(secRad) * r * 0.9f);

  // Hour/minute hands drawn wide (wedge lines) for a bolder look; the second
  // hand stays a thin single-pixel line to keep it visually distinct.
  g->drawWideLine(cx, cy, hx, hy, 2.0f, COL_TEXT);
  g->drawWideLine(cx, cy, mx, my, 1.5f, COL_TEXT);

  // Session (5h) reset time: a thin green radius from center to rim, static
  // (unlike the hands) at whatever hour/minute the session next resets.
  // Drawn before the second hand (below) so the sweeping second hand stays
  // visible on top of it when the two overlap, instead of being masked by it.
  if (haveReset) {
    float resetAngle = ((resetHour24 % 12) + resetMinute / 60.0f) * 30.0f - 90.0f;
    float resetRad = resetAngle * PI / 180.0f;
    int rx = cx + (int)(cosf(resetRad) * (r - 2));
    int ry = cy + (int)(sinf(resetRad) * (r - 2));
    g->drawLine(cx, cy, rx, ry, COL_GOOD);
  }

  g->drawLine(cx, cy, sx, sy, COL_ACCENT);
  g->fillCircle(cx, cy, 2, COL_TEXT);
}

// Card layout: one tall left card = session/week limits (always accent
// orange), with a short BTC price card underneath it (y=192 h=26, using
// the room freed up when drawLimitsCard's 5h card was trimmed to its
// content — see drawLimitsCard's comment); the right column is Bangkok
// analog clock (own row) + digital clock/date (row below) on top (y=2
// h=162, full width to the screen's right edge), then a weather card
// stretched across the same full width (y=166, w=157), given the space
// freed by tightening every gap on this page down to a uniform 2px — every
// card-to-card gap and the outer screen margins are 2px, and the space that
// used to sit idle in those gaps (4-6px each) was instead added to card
// heights so their content gets more breathing room. Left (limits/BTC) and
// right (clock/weather) columns are still 50:50 of the 320px screen: left
// card x=2 w=157 (edge 159), right column x=161 w=157 (edge 318), with the
// 2px column gap between them (159-161) matching the 2px margins at the
// screen's own edges.
static void drawStatusPage() {
  drawLimitsCard();
  drawBtcCard();
  g->fillRect(161, 2, 157, 162, COL_SURFACE);
  g->drawRect(161, 2, 157, 162, COL_BORDER);
  g->fillRect(161, 166, 157, 52, COL_SURFACE);
  g->drawRect(161, 166, 157, 52, COL_BORDER);

  // ── right cards: Bangkok analog + digital clock/date, weather, BTC price ──
  struct tm timeinfo;
  bool haveTime = getLocalTime(&timeinfo, 0);

  long sessionRem = liveResetsInSec(STATE.sessionResetsInSec);
  bool haveReset = haveTime && sessionRem >= 0;
  int resetHour = 0, resetMinute = 0;
  if (haveReset) {
    long totalSec = ((long)timeinfo.tm_hour * 3600 + timeinfo.tm_min * 60 + timeinfo.tm_sec + sessionRem) % 86400;
    resetHour = totalSec / 3600;
    resetMinute = (totalSec % 3600) / 60;
  }

  // Row 1: analog clock, centered on the card's full width. Radius grew
  // 36->44->48->51, the latest bump using the height freed by tightening
  // the gap between the digital time and date rows below from 8px to 2px.
  const int clockCx = 240, clockCy = 61, clockR = 51;
  if (haveTime) {
    drawAnalogClock(clockCx, clockCy, clockR, timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec,
                     haveReset, resetHour, resetMinute);
  } else {
    g->drawCircle(clockCx, clockCy, clockR, COL_TEXT);
  }

  // Row 2: digital time + date, both centered on the card's horizontal
  // midline (241, matching clockCx) since their text width varies with
  // content. Digital time dropped from size4 to size3 to free up vertical
  // room for the bigger analog clock above; date stays size2.
  g->setTextSize(3);
  if (haveTime) {
    char hm[8];
    snprintf(hm, sizeof(hm), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
    int tw = (int)strlen(hm) * 6 * 3;
    g->setCursor(240 - tw / 2, 116);
    g->setTextColor(COL_TEXT);
    g->print(hm);
  } else {
    const char* hm = "--:--";
    int tw = (int)strlen(hm) * 6 * 3;
    g->setCursor(240 - tw / 2, 116);
    g->setTextColor(COL_TEXT2);
    g->print(hm);
  }

  g->setTextColor(COL_TEXT2);
  g->setTextSize(2);
  if (haveTime) {
    // Year dropped: "Mon 25 Jul 2026" at size2 would overrun the 132px
    // inner width; weekday+day+month fits at 120px.
    static const char* mons[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char buf[16];
    snprintf(buf, sizeof(buf), "%s %d %s", WDAY_ABBR[timeinfo.tm_wday], timeinfo.tm_mday,
             mons[timeinfo.tm_mon]);
    int tw = (int)strlen(buf) * 6 * 2;
    g->setCursor(240 - tw / 2, 142);
    g->print(buf);
  } else {
    const char* buf = "--";
    int tw = (int)strlen(buf) * 6 * 2;
    g->setCursor(240 - tw / 2, 142);
    g->print(buf);
  }

  // 1px divider between the "now" icon+temp block and the next-3-hours
  // forecast to its right, centered in the existing gap so it doesn't
  // touch either side's text/icons. (No matching divider on the left —
  // the high/low column reads fine as plain whitespace-separated numbers,
  // and skipping it keeps this side less cluttered.) Sized with an even
  // 6px pad above/below within the taller (52px) card, rather than just
  // shifting the old 34px line, so it uses the extra height freed by the
  // page-wide 2px-gap pass instead of leaving it as dead space.
  g->fillRect(215, 172, 1, 40, COL_BORDER);

  // Today's high/low, in the gap between the card's left edge and the
  // "now" icon — no room for "H"/"L" letter prefixes at this width, so
  // bare numbers stacked high-over-low (bright/grey, same primary-vs-
  // secondary color convention as the rest of the UI) is what fits. x=167
  // leaves a visible left margin (rather than sitting flush against the
  // card border). Rows line up with the icon (top) and temp (bottom)
  // beside them.
  g->setTextSize(1);
  g->setCursor(167, 173);
  g->setTextColor(COL_TEXT);
  g->print(STATE.weatherHigh > -900 ? String(STATE.weatherHigh) : String("--"));
  g->setCursor(167, 198);
  g->setTextColor(COL_TEXT2);
  g->print(STATE.weatherLow > -900 ? String(STATE.weatherLow) : String("--"));

  // Weather card: icon and temp stacked (rather than side by side) since
  // the "now" block was originally sized for a 60px-wide card. The smaller
  // icon (see drawWeatherIcon) is what lets this block fit in the card's
  // height with icon-top-then-temp still snug against both edges. Both are
  // horizontally centered between the high/low column and the divider to
  // the right (x=197), rather than left-anchored, so the block reads as
  // its own column instead of hugging the high/low numbers.
  const int NOW_CENTER_X = 197;
  drawWeatherIcon(NOW_CENTER_X - 8, 174, STATE.weatherCode);
  g->setTextColor(COL_TEXT);
  g->setTextSize(2);
  // No degree glyph in the built-in ASCII font, so just suffix "C" at smaller size and grey color.
  if (STATE.weatherTempC > -900) {
    String tempStr = String((int)round(STATE.weatherTempC));
    int tw = tempStr.length() * 12 + 6;  // digits at size2 + "C" at size1
    g->setCursor(NOW_CENTER_X - tw / 2, 194);
    g->print(tempStr);
    g->setTextSize(1);
    g->setTextColor(COL_TEXT2);
    g->print("C");
  } else {
    g->setCursor(NOW_CENTER_X - 12, 194);  // "--" at size2 = 24px wide
    g->print("--");
  }

  // "Next 3 hours" forecast: three distinct hourly predictions (not just a
  // single reading further out), filling the space freed when the weather
  // card widened to take over the row the BTC card used to share.
  // weatherHourly[] is 1h-stepped starting at the *current* hour (index 0,
  // already shown by the "now" block above), so indices 1..3 are the next
  // three hours. Column layout (hour label, icon, temp) mirrors the
  // Weather page's hourly strip (see drawWeatherPage below). Slot 0 starts
  // close enough to the right divider (x=215) that its icon clears it by
  // only ~5px, closing up the wide gap that used to sit between the "now"
  // temp and the first forecast column.
  const int NEXT_HOUR_SLOT_X = 214, NEXT_HOUR_SLOT_W = 30;
  for (int i = 0; i < 3; i++) {
    int idx = i + 1;
    int sx = NEXT_HOUR_SLOT_X + i * NEXT_HOUR_SLOT_W;
    int cx = sx + NEXT_HOUR_SLOT_W / 2;
    bool have = STATE.weatherHourlyCount > idx;

    char hbuf[4];
    if (have) snprintf(hbuf, sizeof(hbuf), "%02d", STATE.weatherHourly[idx].hour);
    else snprintf(hbuf, sizeof(hbuf), "--");
    g->setTextColor(COL_TEXT2);
    g->setTextSize(1);
    g->setCursor(cx - 6, 171);
    g->print(hbuf);

    drawWeatherIcon(cx - 8, 183, have ? STATE.weatherHourly[idx].code : -1);

    char tbuf[6];
    if (have) snprintf(tbuf, sizeof(tbuf), "%dC", STATE.weatherHourly[idx].tempC);
    else snprintf(tbuf, sizeof(tbuf), "--");
    int tw = (int)strlen(tbuf) * 6;
    g->setTextColor(COL_TEXT);
    g->setCursor(cx - tw / 2, 201);
    g->print(tbuf);
  }
}

static void drawFullStatBlock(int y, const char* title, int percent, const String& sub, uint16_t color) {
  g->setTextColor(COL_TEXT2);
  g->setTextSize(1);
  g->setCursor(10, y);
  g->print(title);

  int barX = 10, barY = y + 14, barW = 246, barH = 16;
  drawPercentBar(barX, barY, barW, barH, percent, color);

  g->setTextColor(COL_TEXT);
  g->setTextSize(2);
  g->setCursor(barX + barW + 8, barY);
  g->print(percent >= 0 ? String(percent) + "%" : "--");

  g->setTextColor(COL_TEXT2);
  g->setTextSize(1);
  g->setCursor(10, barY + 20);
  g->print(sub);
}

// Device Stats: not one of the swiped PAGE_COUNT pages -- reached only by
// tapping the footer's CPU/ROM/RAM stats line (DEVICE_HIT_*), which sets
// devicePageOpen and renders this full-screen, no footer (see render()'s
// devicePageOpen branch, mirrors weatherPageOpen).
static void drawDevicePage() {
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
  int sdPct = cachedSdCapacityPercent(sdUsed, sdTotal);
  uint16_t sdColor = (sdPct >= 80) ? COL_WARN : COL_BLUE;
  drawFullStatBlock(172, "SD CARD USAGE", sdPct,
                     sdPct >= 0 ? fmtGB(sdUsed) + " / " + fmtGB(sdTotal) : "SD CARD NOT FOUND",
                     sdColor);
}

// "OFFLINE" banner overlaid on top of whatever the cat player is already showing (a
// playing cat GIF, or the no-cats placeholder) — a solid bar behind the text
// keeps it legible over a busy GIF frame. Drawn last, right before presentFrame().
void drawOfflineBanner() {
  g->fillRect(0, 0, 320, 44, COL_BG);
  g->setTextColor(COL_TEXT);
  g->setTextSize(5);
  g->setCursor(55, 6);  // centered: "OFFLINE" is 7 chars * 30px = 210px; (320-210)/2 = 55
  g->print("OFFLINE");
}

// Weather detail overlay (status-page weather card). Card-structured landscape
// layout per design.md — no place / HOURLY / 5-DAY word labels (space goes to
// daily row pitch). Hero (temp · H/L · icon · condition) + hourly 6-col +
// 5-day range bars. Content band x=10..320; full 320×240, no footer; any tap
// dismisses. Twin of simulator.html drawWeatherPage — lockstep.
static void drawWeatherPage() {
  // COL_BG (not pure black): borders define structure like the rest of the UI.
  g->fillScreen(COL_BG);

  const int heroRight = 312;  // card right pad (10+310-8)

  // ── Hero card ────────────────────────────────────────────
  // (10, 4, 310, 52) — single inline band fills the card:
  //   icon | size5 temp | size2 condition + size2 H/L
  // Icon leads on the far left, immediately before the current temp.
  g->fillRect(10, 4, 310, 52, COL_SURFACE);
  g->drawRect(10, 4, 310, 52, COL_BORDER);

  // Icon far left, vertically centered in the 52px card (~14–16px tall).
  const int iconX = 16;
  drawWeatherIcon(iconX, 22, STATE.weatherCode);
  const int tempX = iconX + 22;  // past icon box (~18) + gutter

  int metaX = 100;  // start of meta column (after temp)
  g->setTextSize(5);
  g->setCursor(tempX, 10);
  if (STATE.weatherTempC > -900) {
    char tbuf[6];
    snprintf(tbuf, sizeof(tbuf), "%d", (int)round(STATE.weatherTempC));
    g->setTextColor(COL_TEXT);
    g->print(tbuf);
    int tw = (int)strlen(tbuf) * 30;  // size5 advance
    // Raised degree mark (font has no ° glyph) — muted so digits dominate.
    g->setTextSize(2);
    g->setTextColor(COL_TEXT2);
    g->setCursor(tempX + tw, 12);
    g->print("o");
    metaX = tempX + tw + 20;  // past degree + clear gutter into meta column
  } else {
    g->setTextColor(COL_TEXT2);
    g->print("--");
    metaX = tempX + 2 * 30 + 16;
  }

  const int metaMaxX = heroRight;  // full right edge — no trailing icon

  // Condition (size2) — top of meta column, optically with upper temp.
  {
    const char* cond = STATE.weatherCondition[0] ? STATE.weatherCondition : "--";
    int avail = metaMaxX - metaX;
    if (avail < 12) avail = 12;
    int maxChars = avail / 12;  // size2 = 12px/char
    if (maxChars > 14) maxChars = 14;
    if (maxChars < 1) maxChars = 1;
    char cbuf[16];
    int n = 0;
    while (cond[n] && n < maxChars) {
      cbuf[n] = cond[n];
      n++;
    }
    cbuf[n] = 0;
    g->setTextSize(2);
    g->setTextColor(STATE.weatherCondition[0] ? COL_TEXT : COL_TEXT2);
    g->setCursor(metaX, 14);
    g->print(cbuf);
  }

  // High / Low (size2) — "H 35  L 26", same column under condition.
  {
    char hStr[5], lStr[5];
    if (STATE.weatherHigh > -900) snprintf(hStr, sizeof(hStr), "%d", STATE.weatherHigh);
    else snprintf(hStr, sizeof(hStr), "--");
    if (STATE.weatherLow > -900) snprintf(lStr, sizeof(lStr), "%d", STATE.weatherLow);
    else snprintf(lStr, sizeof(lStr), "--");

    g->setTextSize(2);
    int x = metaX;
    g->setTextColor(COL_TEXT2);
    g->setCursor(x, 36);
    g->print("H ");           // label + space (2 cells)
    x += 24;
    g->setTextColor(COL_TEXT);
    g->setCursor(x, 36);
    g->print(hStr);
    x += (int)strlen(hStr) * 12 + 18;  // value + inter-pair gap
    g->setTextColor(COL_TEXT2);
    g->setCursor(x, 36);
    g->print("L ");
    x += 24;
    g->setTextColor(COL_TEXT);
    g->setCursor(x, 36);
    g->print(lStr);
  }

  // ── Hourly (next 6) — no "HOURLY" label ──────────────────
  g->fillRect(10, 60, 310, 56, COL_SURFACE);
  g->drawRect(10, 60, 310, 56, COL_BORDER);

  const int hourCount = STATE.weatherHourlyCount > 0
                          ? (int)STATE.weatherHourlyCount : WEATHER_HOURLY_N;
  // 6 equal columns: 6×51 = 306 (+2px side pad each edge).
  const int slotW = 51;
  for (int i = 0; i < hourCount && i < WEATHER_HOURLY_N; i++) {
    int sx = 12 + i * slotW;
    int cx = sx + slotW / 2;
    int code = -1;
    int temp = 0;
    int hour = -1;
    bool have = (i < (int)STATE.weatherHourlyCount);
    if (have) {
      hour = STATE.weatherHourly[i].hour;
      temp = STATE.weatherHourly[i].tempC;
      code = STATE.weatherHourly[i].code;
    }

    // First slot = current hour: accent tick + accent hour label.
    if (i == 0) {
      g->fillRoundRect(sx + 6, 62, slotW - 12, 2, 1, COL_ACCENT);
    }

    g->setTextSize(1);
    g->setTextColor(i == 0 ? COL_ACCENT : COL_TEXT2);
    char hbuf[4];
    if (have && hour >= 0) snprintf(hbuf, sizeof(hbuf), "%02d", hour);
    else snprintf(hbuf, sizeof(hbuf), "--");
    g->setCursor(cx - 6, 66);
    g->print(hbuf);

    drawWeatherIcon(cx - 8, 78, code);

    if (have) {
      char tbuf[5];
      snprintf(tbuf, sizeof(tbuf), "%d", temp);
      int tw = (int)strlen(tbuf) * 6;
      g->setTextColor(COL_TEXT);
      g->setCursor(cx - tw / 2, 102);
      g->print(tbuf);
    } else {
      g->setTextColor(COL_TEXT2);
      g->setCursor(cx - 6, 102);
      g->print("--");
    }
  }

  // ── 5-day — no "5-DAY" label; extra height → larger dayStep ─
  // Card (10, 122, 310, 114): freed header + section-label rows go here.
  g->fillRect(10, 122, 310, 114, COL_SURFACE);
  g->drawRect(10, 122, 310, 114, COL_BORDER);

  int minT = 100, maxT = -100;
  for (uint8_t i = 0; i < STATE.weatherDailyCount; i++) {
    if (STATE.weatherDaily[i].low < minT) minT = STATE.weatherDaily[i].low;
    if (STATE.weatherDaily[i].high > maxT) maxT = STATE.weatherDaily[i].high;
  }
  if (maxT <= minT) {
    minT = 20;
    maxT = 40;
  }

  // Fixed columns: day 18 | icon 44 | low right@92 | bar 96..272 | high 278
  // dayStep 22: 5×22 = 110 inside 114px card (was 15 in 78px).
  const int dayY0 = 126;
  const int dayStep = 22;
  const int barX = 96, barW = 176, barH = 6;
  const int dayCount = STATE.weatherDailyCount > 0
                         ? (int)STATE.weatherDailyCount : WEATHER_DAILY_N;
  for (int i = 0; i < dayCount && i < WEATHER_DAILY_N; i++) {
    int y = dayY0 + i * dayStep;
    bool have = (i < (int)STATE.weatherDailyCount);
    int wd = have ? STATE.weatherDaily[i].wday : -1;
    int lo = have ? STATE.weatherDaily[i].low : 0;
    int hi = have ? STATE.weatherDaily[i].high : 0;
    int code = have ? STATE.weatherDaily[i].code : -1;

    // Vertically center text/icon/bar in the 22px row.
    const int textY = y + 7;
    const int barY = y + 8;
    const int iconY = y + 4;

    g->setTextSize(1);
    g->setTextColor(i == 0 ? COL_ACCENT : COL_TEXT);
    g->setCursor(18, textY);
    if (have && wd >= 0 && wd < 7) g->print(WDAY_ABBR[wd]);
    else g->print("---");

    drawWeatherIcon(44, iconY, code);

    char lbuf[5], hbuf[5];
    if (have) {
      snprintf(lbuf, sizeof(lbuf), "%d", lo);
      snprintf(hbuf, sizeof(hbuf), "%d", hi);
    } else {
      snprintf(lbuf, sizeof(lbuf), "--");
      snprintf(hbuf, sizeof(hbuf), "--");
    }

    g->setTextColor(COL_TEXT2);
    g->setCursor(barX - 6 - (int)strlen(lbuf) * 6, textY);
    g->print(lbuf);

    g->fillRect(barX, barY, barW, barH, COL_TRACK);
    if (have && maxT > minT) {
      float span = (float)(maxT - minT);
      int x0 = barX + (int)((lo - minT) / span * barW);
      int x1 = barX + (int)((hi - minT) / span * barW);
      if (x1 - x0 < barH) x1 = x0 + barH;
      g->fillRect(x0, barY, x1 - x0, barH, COL_ACCENT);
    }

    g->setTextColor(COL_TEXT);
    g->setCursor(barX + barW + 6, textY);
    g->print(hbuf);
  }
}

void render() {
  // The cat pages are animated frame-by-frame by gifTick() in loop(), not
  // drawn here. Offline mode is also driven by gifTick() (cats + an OFFLINE
  // banner, see catMode in loop()), so render() is never called while
  // offline — no separate offline branch needed here.
  if (currentPage == GIF_PAGE || currentPage == MIXED_PAGE) return;
  uint32_t startUs = micros();  // diagnostic timing only, see comment below
  // Hold the lock across all STATE reads (draw helpers read String members the
  // network task may reassign), then release before the SPI pushSprite so the
  // task's next brief STATE copy isn't delayed by the display write.
  lockState();
  if (weatherPageOpen) {
    drawWeatherPage();
    drawBatterySaveIcon();
    unlockState();
    presentFrame();
    Serial.printf("[timing] render() weather took %luus\n",
                  (unsigned long)(micros() - startUs));
    return;
  }
  if (devicePageOpen) {
    g->fillScreen(COL_BG);
    drawDevicePage();
    drawBatterySaveIcon();
    unlockState();
    presentFrame();
    Serial.printf("[timing] render() device took %luus\n",
                  (unsigned long)(micros() - startUs));
    return;
  }
  g->fillScreen(COL_BG);
  switch (currentPage) {
    case 0: drawStatusPage(); break;
    case 1: drawProjectsPage(); break;  // projects (7d) + 7-day trend combined
    case 2: drawLimitsPage(); break;    // /usage-style limits panel
  }
  drawFooter();
  drawBatterySaveIcon();
  unlockState();
  presentFrame();
  // Baseline timing for the optimization pass that cached flashPercent()'s
  // ESP.getSketchSize() call and retired STATE's hot String fields — kept as
  // a permanent low-volume diagnostic (one line/render, only visible with a
  // serial monitor attached) rather than removed, so a future regression
  // shows up the same way this one was found.
  Serial.printf("[timing] render() page=%d took %luus\n", currentPage,
                (unsigned long)(micros() - startUs));
}
