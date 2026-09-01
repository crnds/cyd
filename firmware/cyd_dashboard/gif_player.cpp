// Page 4 (of 5), CATS/GIF_PAGE: random cat GIFs from /cats/ on the SD card, played endlessly.
// Decode + draw happen on the render core (core 1) in gifTick(); SD reads
// there are guarded by sdMutex so they can't collide with the network task's
// writes. Split out of cyd_dashboard.ino's "PAGE RENDERING"/"SETTINGS"
// sections.
//
// Everything below except the six functions declared in state.h (scanCats,
// gifTick, gifPlayerEnterCatMode/ExitCatMode/ResetForPageChange, and the
// catShuffleMs/catShuffleFixed externs settings.cpp and cyd_dashboard.ino
// read/write) is file-local now — loop()
// used to reach directly into gif/gifOpen/gifNextFrameMs; those raw globals
// are now static here, and loop() calls the three intent-named entry points
// instead.
#include "state.h"

// Dirty-band bounds for the mixed page's partial GIF-region push (declared
// extern in state.h; presentFrame() in pages.cpp reads them).
int gifMinY = 220;
int gifMaxY = -1;

uint32_t catShuffleMs = 0;  // 0 = let each GIF play to its natural end
bool catShuffleFixed = false;  // FIXED preset: never auto-rotate, tap-only advance

// Mixed-page (page 5 of 5, MIXED_PAGE) cat region bounds: starts 2px right of the usage card's
// right edge (card is x=2 w=157, edge=159) and ends 2px short of the screen's
// right edge (161+157=318), mirroring the 2px-gap/2px-margin convention used
// by drawStatusPage's own left/right card columns. Previously this region
// started at x=160 (only 1px clear of the card), which the simulator's
// decorative placeholder border visibly overlapped.
static const int MIXED_GIF_X0 = 161;
static const int MIXED_GIF_W = 157;

// Allocated only while a cat page is showing (see gifPlayerEnterCatMode/
// ExitCatMode) and freed on exit — its work buffers are ~24KB, and keeping
// them resident permanently starved the mbedTLS handshake that the
// BTC/weather HTTPS fetches used to need before those moved server-side.
static AnimatedGIF* gif = nullptr;
static File gifFile;                 // handle the AnimatedGIF file callbacks read through
static const char* CATS_DIR = "/cats";
static const int MAX_CATS = 120;     // cap the in-RAM filename list (names are ~18B each)
static String catFiles[MAX_CATS];
static int catCount = 0;
static int gifXOffset = 0, gifYOffset = 0;  // centering offsets for the current GIF
static bool gifOpen = false;
static bool gifPlaceholderDrawn = false;
static uint32_t gifNextFrameMs = 0;
static uint32_t gifOpenedAtMs = 0;   // when the current GIF opened; used by the shuffle-interval cutoff
static bool gifFirstFrame = false;
static int currentCatIndex = -1;     // currently open cat GIF index

static void drawSessionResetOverlay();
static bool openCatAtIndex(int index, bool resetOpenedTime);
static bool openRandomCat();
static bool reopenCurrentCat();
static void drawGifPlaceholder(bool offline);

// AnimatedGIF file callbacks — the SD reads inside these run under sdMutex,
// which gifTick()/openRandomCat() hold around every gif.open/playFrame/close.
static void* GIFOpenFile(const char* fname, int32_t* pSize) {
  gifFile = SD.open(fname);
  if (!gifFile) return nullptr;
  *pSize = gifFile.size();
  return (void*)&gifFile;
}
static void GIFCloseFile(void* pHandle) {
  File* f = static_cast<File*>(pHandle);
  if (f) f->close();
}
static int32_t GIFReadFile(GIFFILE* pFile, uint8_t* pBuf, int32_t iLen) {
  File* f = static_cast<File*>(pFile->fHandle);
  int32_t want = iLen;
  // Reading to the very last byte breaks a later seek() on this SD lib, so leave one.
  if ((pFile->iSize - pFile->iPos) < iLen) want = pFile->iSize - pFile->iPos - 1;
  if (want <= 0) return 0;
  int32_t got = (int32_t)f->read(pBuf, want);
  pFile->iPos = f->position();
  return got;
}
static int32_t GIFSeekFile(GIFFILE* pFile, int32_t iPosition) {
  File* f = static_cast<File*>(pFile->fHandle);
  f->seek(iPosition);
  pFile->iPos = (int32_t)f->position();
  return pFile->iPos;
}

// One image line into the frame sprite (via `g`), honoring transparency and
// the disposal method — the canonical AnimatedGIF LovyanGFX draw path.
// Centered with the file-local gifXOffset/gifYOffset. presentFrame() is
// called by gifTick() once the whole frame's lines are in.
static void GIFDraw(GIFDRAW* pDraw) {
  uint16_t usTemp[320];
  uint16_t* usPalette = pDraw->pPalette;
  int iWidth = pDraw->iWidth;
  if (iWidth > 320) iWidth = 320;
  int y = gifYOffset + pDraw->iY + pDraw->y;

  bool offline = !STATE.haveData;
  bool mixedMode = (currentPage == MIXED_PAGE && !offline);
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
          if (clipStart < MIXED_GIF_X0) clipStart = MIXED_GIF_X0;
          if (clipEnd > MIXED_GIF_X0 + MIXED_GIF_W) clipEnd = MIXED_GIF_X0 + MIXED_GIF_W;
        } else {
          if (clipStart < 0) clipStart = 0;
          if (clipEnd > 320) clipEnd = 320;
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
      if (clipStart < MIXED_GIF_X0) clipStart = MIXED_GIF_X0;
      if (clipEnd > MIXED_GIF_X0 + MIXED_GIF_W) clipEnd = MIXED_GIF_X0 + MIXED_GIF_W;
    } else {
      if (clipStart < 0) clipStart = 0;
      if (clipEnd > 320) clipEnd = 320;
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
        // Skip hidden files: macOS copies to a FAT SD card leave "._cat_NNN.gif"
        // AppleDouble companions (one per real GIF, created right after it, so
        // FAT enumeration interleaves them) whose names also end in ".gif".
        // Unfiltered they fill half of catFiles[] and then fail gif->open(),
        // silently shrinking the playable library — e.g. only cats 001-060 of
        // 120 ever playing, exactly matching the MAX_CATS cap.
        if (!name.startsWith(".") && lower.endsWith(".gif"))
          catFiles[catCount++] = String(CATS_DIR) + "/" + name;
      }
      f.close();
    }
  }
  if (dir) dir.close();
  unlockSD();
  Serial.printf("[cats] %d GIF(s) in %s\n", catCount, CATS_DIR);
}

// "26% reset: 02:09" pinned to the bottom-left corner of the full-screen
// cat page (GIF_PAGE only, and never while offline -- see the two call
// sites' `&& !offline`), on a solid black box so it stays legible over
// any GIF frame. gifTick() runs unlocked on core 1, so STATE is copied under
// the lock before drawing.
static void drawSessionResetOverlay() {
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
// Drawn once per page visit (gifPlaceholderDrawn) so it doesn't flicker.
static void drawGifPlaceholder(bool offline) {
  bool mixedMode = (currentPage == MIXED_PAGE && !offline);
  if (!gifPlaceholderDrawn) {
    gifPlaceholderDrawn = true;
    if (mixedMode) {
      g->fillRect(MIXED_GIF_X0, 0, MIXED_GIF_W, 240, COL_BG);
      g->setTextColor(COL_ACCENT);
      g->setTextSize(2);
      g->setCursor(215, 92);  // "CATS" = 4 chars * 12px = 48; 161 + (157-48)/2 = 215
      g->print("CATS");
      g->setTextColor(COL_TEXT2);
      g->setTextSize(1);
      const char* msg = "no GIFs";
      g->setCursor(MIXED_GIF_X0 + (MIXED_GIF_W - (int)strlen(msg) * 6) / 2, 120);
      g->print(msg);
    } else {
      g->fillScreen(COL_BG);
      g->setTextColor(COL_ACCENT);
      g->setTextSize(3);
      g->setCursor(124, 92);  // "CATS" = 4 chars * 18px = 72; (320-72)/2 = 124
      g->print("CATS");
      g->setTextColor(COL_TEXT2);
      g->setTextSize(1);
      const char* msg = STATE.sdOk ? "no GIFs found in /cats/ on the SD card"
                                   : "insert an SD card with /cats/ GIFs";
      g->setCursor((320 - (int)strlen(msg) * 6) / 2, 132);
      g->print(msg);
    }
  }
  if (currentPage == GIF_PAGE && !offline) drawSessionResetOverlay();
  drawBatterySaveIcon();
  presentFrame();
}

// Open a cat GIF by index. Centered, honoring page layout.
static bool openCatAtIndex(int index, bool resetOpenedTime) {
  if (catCount == 0 || !gif || index < 0 || index >= catCount) return false;
  gif->begin(GIF_PALETTE_RGB565_BE);  // this panel's pushImage path wants big-endian RGB565 (LE showed swapped colors)
  lockSD();
  int ok = gif->open(catFiles[index].c_str(), GIFOpenFile, GIFCloseFile,
                     GIFReadFile, GIFSeekFile, GIFDraw);
  unlockSD();
  if (!ok) return false;
  int w = gif->getCanvasWidth(), h = gif->getCanvasHeight();
  bool offline = !STATE.haveData;
  if (currentPage == MIXED_PAGE && !offline) {
    gifXOffset = MIXED_GIF_X0 + (MIXED_GIF_W - w) / 2;
    gifYOffset = (220 - h) / 2;
    g->fillRect(MIXED_GIF_X0, 0, MIXED_GIF_W, 240, 0x0000); // clear only the right side
  } else {
    gifXOffset = w < 320 ? (320 - w) / 2 : 0;
    gifYOffset = h < 240 ? (240 - h) / 2 : 0;
    g->fillScreen(0x0000);  // clear the sprite; first frame's lines land on top, then present
  }
  gifOpen = true;
  gifFirstFrame = true;
  if (resetOpenedTime) {
    gifOpenedAtMs = millis();
  }
  return true;
}

// Open a random cat, clearing the sprite first so a smaller GIF letterboxes on
// black rather than over the previous cat. Returns false if the file won't open.
static bool openRandomCat() {
  currentCatIndex = (int)random(catCount);
  return openCatAtIndex(currentCatIndex, true);
}

// Reopen the currently active cat GIF to loop it, keeping the original gifOpenedAtMs.
static bool reopenCurrentCat() {
  return openCatAtIndex(currentCatIndex, false);
}

// Called from loop() on core 1 while a cat page is showing, OR while offline on any
// page (cats double as the offline screen — see catMode in loop()). Decodes at
// most one frame per call (pacing itself via gifNextFrameMs) so touch stays
// responsive; when a GIF ends it immediately opens another at random — endless
// cats.
void gifTick(bool offline) {
  if (!STATE.sdOk || catCount == 0 || !gif) { drawGifPlaceholder(offline); return; }
  uint32_t now = millis();
  // (int32_t) subtraction wraps correctly across millis()'s ~49.7-day
  // rollover, unlike a direct now < gifNextFrameMs comparison.
  if (gifOpen && (int32_t)(now - gifNextFrameMs) < 0) return;  // not time for the next frame yet
  if (!gifOpen && !openRandomCat()) {
    drawGifPlaceholder(offline);
    gifNextFrameMs = now + 1000;  // retry opening later
    return;
  }
  now = millis(); // Refresh now since openRandomCat() performs slow SD I/O
  int delayMs = 0;
  if (gifFirstFrame) {
    gifMinY = 0;
    gifMaxY = 219;
  } else {
    gifMinY = 220;
    gifMaxY = -1;
  }
  // Bounded wait: if networkTask (core 0) is mid-poll on the card, drop this
  // frame and try again shortly rather than blocking the render core and
  // holding the bus hostage. See tryLockSD()'s comment in state.h — during an
  // outage the cat player and the recovery poll are competing for the same
  // card, and the poll has to win.
  if (!tryLockSD(20)) {
    gifNextFrameMs = now + 40;
    return;
  }
  int more = gif->playFrame(false, &delayMs);  // bSync=false: we handle timing ourselves
  unlockSD();
  if (gifFirstFrame) {
    gifFirstFrame = false;
  }
  if (currentPage == GIF_PAGE && !offline) drawSessionResetOverlay();
  drawBatterySaveIcon();
  if (currentPage == MIXED_PAGE && !offline) {
    // Battery Save's corner box sits inside the GIF region but outside
    // whatever rows this frame's GIFDraw touched — fold it into the dirty
    // band so the icon actually reaches the panel via the partial push below.
    if (BATTERY_ICON_Y0 < gifMinY) gifMinY = BATTERY_ICON_Y0;
    if (BATTERY_ICON_Y1 > gifMaxY) gifMaxY = BATTERY_ICON_Y1;
  }
  if (mixedPageDirty) {
    presentFrame(true);  // flush the static card update
    mixedPageDirty = false;
  } else {
    presentFrame(false); // push only the GIF region
  }
  // Rotate to a new random cat either at the GIF's natural end, or early if
  // the Settings-page shuffle interval says this one has played long enough
  // (catShuffleMs == 0 disables the early cutoff -- always play to the end).
  // FIXED (catShuffleFixed) disables auto-rotation entirely -- the current
  // cat just keeps replaying (the `more == 0` branch below) until a manual
  // tap calls gifPlayerResetForPageChange().
  bool shuffleDue = !catShuffleFixed && catShuffleMs > 0 && now >= gifOpenedAtMs && (now - gifOpenedAtMs) >= catShuffleMs;
  if (shuffleDue || (more == 0 && !catShuffleFixed && catShuffleMs == 0)) {
    lockSD(); gif->close(); unlockSD();
    gifOpen = false;
    gifNextFrameMs = now;          // open the next one on the following tick
  } else if (more == 0) {
    lockSD(); gif->close(); unlockSD();
    gifOpen = false;
    if (reopenCurrentCat()) {
      gifNextFrameMs = now;        // reopen successful, play first frame next tick
    } else {
      gifNextFrameMs = now;        // fallback to random if error
    }
  } else {
    gifNextFrameMs = now + (delayMs > 0 ? delayMs : 80);
  }
}

// Allocate the decoder only while it's needed (entering a cat page, or going
// offline on any page) and reset the placeholder/timer for the fresh visit.
void gifPlayerEnterCatMode() {
  if (!gif) gif = new AnimatedGIF();
  gifPlaceholderDrawn = false;
  gifNextFrameMs = 0;
}

// Leaving cat mode: close any open GIF and free the decoder's ~24KB.
void gifPlayerExitCatMode() {
  if (gif && gifOpen) { lockSD(); gif->close(); unlockSD(); }
  gifOpen = false;
  delete gif;
  gif = nullptr;
}

// Force the next gifTick() to open a fresh (random) GIF — used when the
// touch handler navigates directly onto GIF_PAGE/MIXED_PAGE.
void gifPlayerResetForPageChange() {
  if (gifOpen) { lockSD(); gif->close(); unlockSD(); }
  gifOpen = false;
  gifNextFrameMs = 0;
}
