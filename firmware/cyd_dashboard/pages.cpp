// Frame presentation (pixel-shift, dirty-band pushes) + every full-screen
// page's draw function. This is the near-line-for-line twin of
// simulator.html's page-drawing functions — see the parent CLAUDE.md's
// firmware/simulator parity rule before touching layout, colors, or text
// here without updating the simulator too. Split out of cyd_dashboard.ino.
#include "state.h"

// Touch feedback: flash a 5px white border on the left or right part of the screen
// for one frame, straight to the panel over the already-pushed frame, then re-push
// the clean frame to clear it. Drawn on `gfx` (not `g`) so it overlays the
// composited page; presentFrame() restores the borderless frame.
void flashTouchBorder(bool isRight) {
  const int T = 5;
  const uint16_t WHITE = 0xFFFF;
  if (isRight) {
    gfx.fillRect(152, 0, 152, T, WHITE);        // top right
    gfx.fillRect(152, 240 - T, 152, T, WHITE);  // bottom right
    gfx.fillRect(304 - T, 0, T, 240, WHITE);    // right
  } else {
    gfx.fillRect(0, 0, 152, T, WHITE);          // top left
    gfx.fillRect(0, 240 - T, 152, T, WHITE);    // bottom left
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

void presentFrame(bool fullScreen) {
  bool isEvenSecond = false;
  if (checkHourlyFlash(isEvenSecond)) {
    gfx.invertDisplay(isEvenSecond);
  } else {
    gfx.invertDisplay(false);
  }

  if (g == &frame) {
    // Clear rightmost 16px (5% of 320) to black so it's always blank/padded
    frame.fillRect(304, 0, 16, 240, 0x0000);

    bool offline = !STATE.haveData;
    // All destination coords carry the pixel-shift offset; LovyanGFX clips
    // pushes past the panel edge, so no bounds checks are needed. After an
    // orbit step (shiftDirty) the partial path would leave half the screen at
    // the old offset, so that one frame falls through to the full blit below.
    if (currentPage == MIXED_PAGE && !offline && !shiftDirty) {
      gfx.startWrite();
      if (frame.getColorDepth() == 16) {
        uint16_t* buf = (uint16_t*)frame.getBuffer();
        if (fullScreen) {
          // Push left half (0-159, height 240) + right footer (160-303, rows 220-239)
          for (int y = 0; y < 240; y++) {
            gfx.pushImage(shiftX, y + shiftY, 160, 1, buf + y * 320);
          }
          for (int y = 220; y < 240; y++) {
            gfx.pushImage(160 + shiftX, y + shiftY, 144, 1, buf + y * 320 + 160);
          }
        } else {
          // Push only the dirty band of the right half (160-303) where the GIF is drawn
          if (gifMinY <= gifMaxY) {
            int startY = gifMinY < 0 ? 0 : gifMinY;
            int endY = gifMaxY >= 220 ? 219 : gifMaxY;
            for (int y = startY; y <= endY; y++) {
              gfx.pushImage(160 + shiftX, y + shiftY, 144, 1, buf + y * 320 + 160);
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
            gfx.pushImage(shiftX, y + shiftY, 160, 1, rowBuf);
          }
          // Push right footer (160-303, rows 220-239)
          for (int y = 220; y < 240; y++) {
            frame.readRect(160, y, 144, 1, rowBuf);
            gfx.pushImage(160 + shiftX, y + shiftY, 144, 1, rowBuf);
          }
        } else {
          // Push only the dirty band of the right half (160-303)
          if (gifMinY <= gifMaxY) {
            int startY = gifMinY < 0 ? 0 : gifMinY;
            int endY = gifMaxY >= 220 ? 219 : gifMaxY;
            for (int y = startY; y <= endY; y++) {
              frame.readRect(160, y, 144, 1, rowBuf);
              gfx.pushImage(160 + shiftX, y + shiftY, 144, 1, rowBuf);
            }
          }
        }
      }
      gfx.endWrite();
    } else {
      frame.pushSprite(shiftX, shiftY);
      clearShiftMargins();
      shiftDirty = false;
    }
  }
}

// ── DRAWING HELPERS ────────────────────────────────────────
void drawFooter() {
  // Status dot: blinks once a second while the most recent poll reached the
  // server — green on the odd second, off on the even second, so the pulse is
  // easy to read at a glance. Static amber when coasting on cached data
  // (haveData true but connected false — render() only gets here when
  // haveData is true, so there's no "no data" case left to color for).
  bool onPhase = (millis() / 1000) % 2;
  if (!connected) g->fillCircle(14, 230, 3, COL_ACCENT);
  else if (onPhase) g->fillCircle(14, 230, 4, COL_GOOD);

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

static void drawLimitBlock(int y, const char* title, int percent, const char* resets, long resetsInSec) {
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
  g->setCursor(294 - (int)right.length() * 6, y);
  g->print(right);
  g->fillRoundRect(10, y + 12, 284, 8, 2, COL_TRACK);
  if (percent >= 0) {
    int fillW = (int)((float)min(percent, 100) / 100 * 284 + 0.5f);
    g->fillRoundRect(10, y + 12, max(fillW, 3), 8, 2, COL_ACCENT);
  }
}

// "Resets Jul 16, 04:59  58%" — or bare "58%", or "--" when unknown.
static String limitRowText(int percent, const char* resets) {
  if (percent < 0) return "--";
  String s;
  if (resets[0] != '\0') s = String("Resets ") + resets + "  ";
  s += String(percent) + "%";
  return s;
}

// Page 6: the Claude Code /usage panel — context window, 5-hour limit,
// weekly (all models), weekly per-model (hidden when the server sends null,
// e.g. if the per-model limit is ever discontinued), usage credits. Rows
// shift up when a row is absent.
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
  // Tick the session countdown down locally between polls.
  long sessionRem = STATE.sessionResetsInSec;
  if (sessionRem >= 0) {
    sessionRem -= (long)((millis() - STATE.lastFetchOkMs) / 1000);
    if (sessionRem < 0) sessionRem = 0;
  }
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
// from circles/rects/lines, mapped from Open-Meteo's WMO weather_code. cx,cy
// is the center of a ~24x20px icon anchored at (x,y).
// Scaled down (~0.65x) from the original so the weather card can shrink to
// match the BTC card's minimum content height (see drawStatusPage).
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

// Thin inset progress bar: near-black track recessed into a surface card.
// Returns the fill width in px (-1 when percent is unknown) so callers can
// overlay effects on the filled region (the green bars' shine sweep below).
static int drawMiniBar(int x, int y, int w, int percent, uint16_t color, uint16_t trackColor = COL_TRACK, int h = 6) {
  g->fillRoundRect(x, y, w, h, h / 2, trackColor);
  if (percent < 0) return -1;
  int fillW = max((int)((float)min(percent, 100) / 100 * w), h);
  g->fillRoundRect(x, y, fillW, h, h / 2, color);
  return fillW;
}

// ── SHINE SWEEP ────────────────────────────────────────────
// Looping light band swept across the green reset-countdown bars' fill in
// drawLimitsCard (pages 1 and 8). The band crosses the full 122px track at
// constant speed but is only painted over the filled interior, so short
// fills just see it pass through. Twin of simulator.html's drawShineStrip.
static const uint32_t SHINE_PERIOD_MS = 2600;
static const int SHINE_BAND_R = 7;
static const int SHINE_BAR_X = 12, SHINE_BAR_W = 122, SHINE_BAR_H = 4;
static const int SHINE_BAR_Y[2] = {61, 182};  // 5h, weekly (drawLimitsCard)
// Fill widths cached by drawLimitsCard (under stateMutex) so shineTick can
// repaint between renders without touching STATE — same lock-free-volatile
// pattern as loop()'s poll-progress line.
static volatile int shineFillPx[2] = {-1, -1};

// Paint the shine band over one bar's filled interior on `dst` — the sprite
// during drawLimitsCard (so pushed frames already carry the band), or the
// panel directly from shineTick (caller adds the pixel-shift offset).
// Interior columns only (2..fillW-3, full height) so the radius-2 rounded
// ends stay untouched; repainting non-band columns COL_GOOD erases the trail.
static void drawShineStrip(lgfx::LovyanGFX* dst, int x, int y, int fillW, uint32_t nowMs) {
  if (fillW < 12) return;
  float phase = (float)(nowMs % SHINE_PERIOD_MS) / SHINE_PERIOD_MS;
  int center = -SHINE_BAND_R + (int)(phase * (SHINE_BAR_W + 2 * SHINE_BAND_R));
  dst->startWrite();
  for (int i = 2; i <= fillW - 3; i++) {
    int d = abs(i - center);
    uint16_t c = d <= 1 ? COL_SHINE_HI : d <= 4 ? COL_SHINE_MID : d <= SHINE_BAND_R ? COL_SHINE_LO : COL_GOOD;
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
    drawShineStrip(&gfx, SHINE_BAR_X + shiftX, SHINE_BAR_Y[i] + shiftY, shineFillPx[i], nowMs);
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
  g->fillRoundRect(2, 4, 142, 216, 8, COL_SURFACE);
  g->drawRoundRect(2, 4, 142, 216, 8, COL_BORDER);

  // Tick both countdowns down locally between polls.
  long sessionRem = STATE.sessionResetsInSec;
  if (sessionRem >= 0) {
    sessionRem -= (long)((millis() - STATE.lastFetchOkMs) / 1000);
    if (sessionRem < 0) sessionRem = 0;
  }
  long weekRem = STATE.weekResetsInSec;
  if (weekRem >= 0) {
    weekRem -= (long)((millis() - STATE.lastFetchOkMs) / 1000);
    if (weekRem < 0) weekRem = 0;
  }

  // ── left card: limits ──
  String sessionPctStr = STATE.sessionPercent >= 0 ? String(STATE.sessionPercent) + "%" : "--";
  g->setTextColor(COL_ACCENT);
  g->setTextSize(4);
  g->setCursor(12, 13);
  g->print(sessionPctStr);
  drawCardLabel(12 + sessionPctStr.length() * 24 + 6, 37, "5H");

  drawMiniBar(12, 51, 122, STATE.sessionPercent, COL_ACCENT);
  // Green reset-countdown bars (and their shine) are optional via Settings
  // "Show Countdown"; when off, clear the cached fill so shineTick no-ops.
  if (cfgShowCountdown) {
    shineFillPx[0] = drawMiniBar(12, 61, 122, elapsedPercentOfWindow(sessionRem, SESSION_WINDOW_SEC), COL_GOOD, COL_TRACK_BLACK, 4);
    drawShineStrip(g, SHINE_BAR_X, SHINE_BAR_Y[0], shineFillPx[0], millis());
  } else {
    shineFillPx[0] = -1;
  }

  g->setTextColor(COL_TEXT2);
  g->setTextSize(2);
  g->setCursor(12, 75);
  g->print("resets:");
  g->setCursor(12, 93);
  g->print(STATE.sessionResets[0] != '\0' ? STATE.sessionResets : "--");
  g->setTextColor(COL_TEXT);
  g->setTextSize(2);
  g->setCursor(12, 111);
  if (sessionRem >= 0) g->print("in " + fmtCountdown(sessionRem));

  g->fillRect(12, 133, 122, 1, COL_BORDER);

  String weekPctStr = STATE.weekPercent >= 0 ? String(STATE.weekPercent) + "%" : "--";
  g->setTextColor(COL_ACCENT);
  g->setTextSize(3);
  g->setCursor(12, 142);
  g->print(weekPctStr);
  drawCardLabel(12 + weekPctStr.length() * 18 + 6, 158, "WEEK");

  drawMiniBar(12, 172, 122, STATE.weekPercent, COL_ACCENT);
  if (cfgShowCountdown) {
    shineFillPx[1] = drawMiniBar(12, 182, 122, elapsedPercentOfWindow(weekRem, WEEK_WINDOW_SEC), COL_GOOD, COL_TRACK_BLACK, 4);
    drawShineStrip(g, SHINE_BAR_X, SHINE_BAR_Y[1], shineFillPx[1], millis());
  } else {
    shineFillPx[1] = -1;
  }

  // "resets Jul 9, 4:59am" is 20 chars = 120px at size1 — comfortably under the
  // card's inner width, so this line must stay size1.
  g->setTextColor(COL_TEXT2);
  g->setTextSize(1);
  g->setCursor(12, 196);
  g->print(STATE.weekResets[0] != '\0' ? String("resets ") + STATE.weekResets : String("resets --"));

  g->setTextColor(COL_TEXT);
  g->setTextSize(1);
  g->setCursor(12, 206);
  if (weekRem >= 0) g->print("in " + fmtCountdownDHM(weekRem));
}

void drawMixedPageStatic() {
  // Clear the footer background area to prevent text bloating/overlapping
  g->fillRect(0, 220, 320, 20, COL_BG);
  drawLimitsCard();
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
  g->drawLine(cx, cy, sx, sy, COL_ACCENT);
  g->fillCircle(cx, cy, 2, COL_TEXT);

  // Session (5h) reset time: a thin green radius from center to rim, static
  // (unlike the hands) at whatever hour/minute the session next resets.
  // Floating on top of other hands.
  if (haveReset) {
    float resetAngle = ((resetHour24 % 12) + resetMinute / 60.0f) * 30.0f - 90.0f;
    float resetRad = resetAngle * PI / 180.0f;
    int rx = cx + (int)(cosf(resetRad) * (r - 2));
    int ry = cy + (int)(sinf(resetRad) * (r - 2));
    g->drawLine(cx, cy, rx, ry, COL_GOOD);
  }
}

// Card layout: one tall left card = session/week limits (always accent
// orange); the right column is Bangkok analog clock (own row) + digital
// clock/date (row below) on top (y=4 h=162), then a bottom row of two
// side-by-side cards = weather (y=170 w=60) and BTC (y=170 w=86), both
// trimmed to 46px — the minimum that still fits the (now smaller)
// weather icon + temp text, which is the taller of the two — to give the
// clock the extra room. 4px gaps throughout.
static void drawStatusPage() {
  drawLimitsCard();
  g->fillRoundRect(150, 4, 150, 162, 8, COL_SURFACE);
  g->drawRoundRect(150, 4, 150, 162, 8, COL_BORDER);
  g->fillRoundRect(150, 170, 60, 46, 8, COL_SURFACE);
  g->drawRoundRect(150, 170, 60, 46, 8, COL_BORDER);
  g->fillRoundRect(214, 170, 86, 46, 8, COL_SURFACE);
  g->drawRoundRect(214, 170, 86, 46, 8, COL_BORDER);

  // ── right cards: Bangkok analog + digital clock/date, weather, BTC price ──
  struct tm timeinfo;
  bool haveTime = getLocalTime(&timeinfo, 0);

  // Tick the session countdown down locally between polls, same as
  // drawLimitsCard/drawHomePage, to find the wall-clock time it resets at.
  long sessionRem = STATE.sessionResetsInSec;
  if (sessionRem >= 0) {
    sessionRem -= (long)((millis() - STATE.lastFetchOkMs) / 1000);
    if (sessionRem < 0) sessionRem = 0;
  }
  bool haveReset = haveTime && sessionRem >= 0;
  int resetHour = 0, resetMinute = 0;
  if (haveReset) {
    long totalSec = ((long)timeinfo.tm_hour * 3600 + timeinfo.tm_min * 60 + timeinfo.tm_sec + sessionRem) % 86400;
    resetHour = totalSec / 3600;
    resetMinute = (totalSec % 3600) / 60;
  }

  // Row 1: analog clock, centered on the card's full width. Radius grew
  // 36->44 using the height freed by shrinking the weather/BTC row below.
  const int clockCx = 225, clockCy = 56, clockR = 44;
  if (haveTime) {
    drawAnalogClock(clockCx, clockCy, clockR, timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec,
                     haveReset, resetHour, resetMinute);
  } else {
    g->drawCircle(clockCx, clockCy, clockR, COL_TEXT);
  }

  // Row 2: digital time + date, back to their original size4/size2 (the
  // clock card is tall enough now that they no longer need to share a row).
  g->setTextSize(4);
  g->setCursor(160, 104);
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
    g->setCursor(160 + 5 * 24, 128);
    g->print(ss);
  } else {
    g->setTextColor(COL_TEXT2);
    g->print("--.--");
  }

  g->setTextColor(COL_TEXT2);
  g->setTextSize(2);
  g->setCursor(160, 144);
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

  // Weather card: icon and temp stacked (rather than side by side) since
  // the card is only 60px wide. The smaller icon (see drawWeatherIcon) is
  // what lets this card fit in the new 46px height with icon-top-then-temp
  // still snug against both edges.
  drawWeatherIcon(169, 175, STATE.weatherCode);
  g->setTextColor(COL_TEXT);
  g->setTextSize(2);
  g->setCursor(165, 195);
  // No degree glyph in the built-in ASCII font, so just suffix "C" at smaller size and grey color.
  if (STATE.weatherTempC > -900) {
    g->print(String((int)round(STATE.weatherTempC)));
    g->setTextSize(1);
    g->setTextColor(COL_TEXT2);
    g->print("C");
  } else {
    g->print("--");
  }

  // BTC card: label+price pushed down to the card's bottom edge (mirroring
  // where the weather card's temp text sits) instead of leaving a gap below
  // a top-anchored block — this card's content is shorter than weather's.
  drawCardLabel(224, 184, "BTC/USDT");

  g->setTextColor(COL_TEXT);
  g->setTextSize(2);
  g->setCursor(221, 196);
  g->print(fmtBtc(STATE.btcPrice));
}

static void drawLongTrendPage() {
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
  // so on its own it stops at yesterday — append one more bar so the rightmost
  // bar matches the "today" label. It uses last24hTokens (a rolling trailing
  // 24h sum), not trend[6] (midnight-to-now): trend[6] is a partial total that
  // starts near zero at midnight and only represents a full day right before
  // rollover, which would make every other, already-complete day's bar look
  // inconsistent with it for most of the day.
  int64_t bars[LONG_TREND_DAYS];
  int barCount = longTrendCount;
  for (int i = 0; i < barCount; i++) bars[i] = longTrend[i];
  if (barCount < LONG_TREND_DAYS) {
    bars[barCount++] = STATE.last24hTokens;
  } else {
    memmove(bars, bars + 1, (LONG_TREND_DAYS - 1) * sizeof(int64_t));
    bars[LONG_TREND_DAYS - 1] = STATE.last24hTokens;
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

static void drawFullStatBlock(int y, const char* title, int percent, const String& sub, uint16_t color) {
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
  int sdPct = sdCapacityPercent(sdUsed, sdTotal);
  uint16_t sdColor = (sdPct >= 80) ? COL_WARN : COL_BLUE;
  drawFullStatBlock(172, "SD CARD USAGE", sdPct,
                     sdPct >= 0 ? fmtGB(sdUsed) + " / " + fmtGB(sdTotal) : "SD CARD NOT FOUND",
                     sdColor);
}

// "OFFLINE" banner overlaid on top of whatever the cat player is already showing (a
// playing cat GIF, or the no-cats placeholder) — a solid bar behind the text
// keeps it legible over a busy GIF frame. Drawn last, right before presentFrame().
void drawOfflineBanner() {
  g->fillRect(0, 0, 304, 44, COL_BG);
  g->setTextColor(COL_TEXT);
  g->setTextSize(5);
  g->setCursor(47, 6);  // centered: "OFFLINE" is 7 chars * 30px = 210px; (304-210)/2 = 47
  g->print("OFFLINE");
}

// Weather detail overlay (status-page weather card). Card-structured landscape
// layout per design.md — no place / HOURLY / 5-DAY word labels (space goes to
// daily row pitch). Hero (temp · H/L · icon · condition) + hourly 6-col +
// 5-day range bars. Content band x=10..294; full 304×240, no footer; any tap
// dismisses. Twin of simulator.html drawWeatherPage — lockstep.
static void drawWeatherPage() {
  // COL_BG (not pure black): borders define structure like the rest of the UI.
  g->fillScreen(COL_BG);

  static const char* WDAYS[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  const int heroRight = 286;  // card right pad (10+284-8)

  // ── Hero card ────────────────────────────────────────────
  // (10, 4, 284, 52) — single inline band fills the card:
  //   icon | size5 temp | size2 condition + size2 H/L
  // Icon leads on the far left, immediately before the current temp.
  g->fillRoundRect(10, 4, 284, 52, 8, COL_SURFACE);
  g->drawRoundRect(10, 4, 284, 52, 8, COL_BORDER);

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
  g->fillRoundRect(10, 60, 284, 56, 8, COL_SURFACE);
  g->drawRoundRect(10, 60, 284, 56, 8, COL_BORDER);

  const int hourCount = STATE.weatherHourlyCount > 0
                          ? (int)STATE.weatherHourlyCount : WEATHER_HOURLY_N;
  // 6 equal columns: 6×47 = 282 (+1px side pad each edge).
  const int slotW = 47;
  for (int i = 0; i < hourCount && i < WEATHER_HOURLY_N; i++) {
    int sx = 11 + i * slotW;
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
  // Card (10, 122, 284, 114): freed header + section-label rows go here.
  g->fillRoundRect(10, 122, 284, 114, 8, COL_SURFACE);
  g->drawRoundRect(10, 122, 284, 114, 8, COL_BORDER);

  int minT = 100, maxT = -100;
  for (uint8_t i = 0; i < STATE.weatherDailyCount; i++) {
    if (STATE.weatherDaily[i].low < minT) minT = STATE.weatherDaily[i].low;
    if (STATE.weatherDaily[i].high > maxT) maxT = STATE.weatherDaily[i].high;
  }
  if (maxT <= minT) {
    minT = 20;
    maxT = 40;
  }

  // Fixed columns: day 18 | icon 44 | low right@92 | bar 96..246 | high 252
  // dayStep 22: 5×22 = 110 inside 114px card (was 15 in 78px).
  const int dayY0 = 126;
  const int dayStep = 22;
  const int barX = 96, barW = 150, barH = 6;
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
    if (have && wd >= 0 && wd < 7) g->print(WDAYS[wd]);
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

    g->fillRoundRect(barX, barY, barW, barH, barH / 2, COL_TRACK);
    if (have && maxT > minT) {
      float span = (float)(maxT - minT);
      int x0 = barX + (int)((lo - minT) / span * barW);
      int x1 = barX + (int)((hi - minT) / span * barW);
      if (x1 - x0 < barH) x1 = x0 + barH;
      g->fillRoundRect(x0, barY, x1 - x0, barH, barH / 2, COL_ACCENT);
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
    unlockState();
    presentFrame();
    Serial.printf("[timing] render() weather took %luus\n",
                  (unsigned long)(micros() - startUs));
    return;
  }
  g->fillScreen(COL_BG);
  switch (currentPage) {
    case 0: drawStatusPage(); break;
    case 1: drawProjectsPage(); break;  // projects (7d) + 7-day trend combined
    case 2: drawHomePage(); break;
    case 3: drawDevicePage(); break;
    case 4: drawLongTrendPage(); break;
    case 5: drawLimitsPage(); break;    // /usage-style limits panel
  }
  drawFooter();
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
