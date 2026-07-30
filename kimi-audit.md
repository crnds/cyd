# Full-project audit — 2026-07-30

Scope: `server/*.py` + plists, `firmware/cyd_dashboard/*`, `simulator.html`, `server.html`, helper scripts, docs, and repo hygiene. Every finding below was verified against the cited code.

## HIGH severity

1. **Hardcoded live GIPHY API key committed to git** — `pull_giphy_cats.py:29` (`DEFAULT_API_KEY = "qYVB…"`), committed in `10238c6` and present on `origin/HEAD` (`origin/feat/sd-cats-and-market-proxy`). If the repo is public, the key is exposed.
   Fix: rotate the key in the GIPHY dashboard, read from env / `secrets.local.json`, purge from history (or accept leak after rotation).

2. **Stale drag state traps the Settings UI** — `settings.cpp:421–463` + `cyd_dashboard.ino:436–438`: `settingsListDragBegin()` is only called for taps landing while already in `SET_LIST`, not when entering it. The entry tap's release then hits `settingsListDragEnd()` with stale `dragLastX/Y`: first-ever pulse-dot tap closes settings on finger-lift; leaving a leaf via BACK re-opens the same leaf on release (user is trapped).
   Fix: call `settingsListDragBegin(tx, ty)` at every transition into `SET_LIST` (or add a "gesture begun" flag `dragEnd` requires). Also: `settingsListDragMove` (settings.cpp:429–431) updates `dragLastX` but not `dragLastY` when `dy==0`.

3. **SD bus accessed on render core without mutex** — `pages.cpp:1189–1197` → `934–938` → `format.cpp:99–106`: `render()` holds `stateMutex` while `drawDevicePage()` calls `sdCapacityPercent()` → `SD.totalBytes()/usedBytes()` on core 1, racing networkTask (core 0) mid-write on the same HSPI bus. Can't be fixed by adding `lockSD()` there (would be the forbidden `stateMutex → sdMutex` nesting).
   Fix: cache SD capacity in networkTask (under `sdMutex`) into volatile globals that `drawDevicePage()` reads.

4. **"Reset to auto" battery-save button is broken** — `server.html:466`: `postBatterySave(null)` sends `{"enabled": false}` (because of `!!enabled`), forcing manual-OFF instead of clearing the override. Server chain supports null end-to-end (`control_server.py:199-207`, `usage_server.py:128-138`); only the client coerces it away.
   Fix: `body: JSON.stringify({ enabled: enabled === null ? null : !!enabled })`.

## MEDIUM severity

5. **Drive-by CSRF on the control server** — `control_server.py:143,153–157`: `Access-Control-Allow-Origin: *` on every response + preflights granted to any origin; any web page the user visits can POST `http://127.0.0.1:8788/api/disable` and bootout the usage server (simple POST needs no preflight). Same pattern on `usage_server.py:1066` (ACAO `*` exposes usage data cross-origin) and `usage_server.py:1092–1094` (localhost-gated `POST /api/battery-save` is browser-reachable).
   Fix: reject POSTs whose `Origin` isn't `http://127.0.0.1:8788` (or require a custom header that forces a preflight); emit ACAO only for `Origin: null` (the `file://` simulator) or drop it on the control server.

6. **Weather API key sent over cleartext HTTP** — `usage_server.py:589`: `http://api.weatherapi.com/...?key=...` exposes `WEATHER_API_KEY` on the network path.
   Fix: use `https://`.

7. **`cats_raw/` not gitignored** — `pull_giphy_cats.py:172-173` defaults to a `cats_raw/` dir targeting ~2.5 GB; one `git add -A` would try to commit it.
   Fix: add `cats_raw/` (and `.DS_Store`) to `.gitignore`.

8. **`prepare_cat_gifs.py` crashes mid-batch on truncated downloads** — `prepare_cat_gifs.py:133–137`: `http.client.IncompleteRead` from `resp.read()` is not caught (not a subclass of `URLError`/`TimeoutError`/`OSError`).
   Fix: catch `http.client.IncompleteRead` in `download_gif()` and return `None`.

## LOW severity

### Server
- `usage_server.py:1153` — initial `start_http_server()` outside try; `EADDRINUSE` crash-loops launchd every ~10s. Wrap/log/retry or add `ThrottleInterval`.
- `usage_server.py:811,839–843` — naive-timestamp `TypeError` after offsets already advanced permanently drops other files' new events. Normalize naive `ts` to UTC / advance offsets per-file in try.
- `usage_server.py:47,843` — `STATE["offsets"]` never pruned (grows for life of process). Drop dead paths in retention sweep.
- `usage_server.py:1081` + plist — `/tmp/cydusage.log` grows ~200KB/day, no rotation. Log heartbeats at most once per N minutes.
- `usage_server.py:1096–1099`, `control_server.py:190–193` — unbounded `Content-Length` read (slow-loris). Cap at ~4KB, 413 otherwise.
- `usage_server.py:544–549` — Open-Meteo hourly fallback shows oldest 6h at forecast-window tail. Init `start_i = len(h_times)`.
- `usage_server.py:136` — `activity_event.set()` doesn't actually wake loops (they `time.sleep`); document or switch to `Event.wait`.

### Firmware
- `net.cpp:437–441` — double JSON parse per poll (two elastic `JsonDocument` allocations). Add `applyUsageJson(const JsonDocument&)` overload.
- `state.h:233` — `STATE.ctxTokens` is 32-bit `long`; make `int64_t` per the token-width invariant.
- `gif_player.cpp:312` — `millis()`-wrap-unsafe frame pacing; use `(int32_t)(now - gifNextFrameMs) < 0`.
- `settings.cpp:58–64`/`net.cpp:254` — `cfgBatterySaveMode`/`cfgPollIntervalSec` read cross-core, not `volatile`.
- `ap_setup.cpp:60–63` — scanned SSIDs interpolated into setup-page HTML unescaped.
- `settings.cpp:79` — `applyRestart()` restarts on core 1 possibly mid-SD-write; route through a `pendingRestart` drained by networkTask.
- `cyd_dashboard.ino:125` — stale "~15 min" comment (with Battery Save's 120s floor it's ~90 min).

### Simulator / control panel
- `simulator.html:780` — footer page label ignores `FOOTER_RIGHT_PADDING=8` (8px too far right).
- `simulator.html:1687` — offline draws session-reset overlay on every page; firmware gates it on `GIF_PAGE`.
- `simulator.html:835,939,964,1047,1588–1589,1609` — `Math.round` where firmware truncates (±1px bar drift). Use `Math.floor`.
- `simulator.html:2214` — no in-flight guard on `fetchUsage` (concurrent fetches skew state/footer).
- `simulator.html:208` — hardcoded "page 1/6" flash though `PAGE_COUNT = 5`.
- `server.html:639–658` — unescaped `innerHTML` from server strings (near-impossible injection on localhost, but breaks the file's own safe pattern).

### Docs / hygiene
- `design.md` — claims "Accepted (as-built)", describes 8 pages + BTC tiles; BTC removed in `af2e232`, UI changed in `a51cafe`. Add a superseded banner or update.
- `CLAUDE.md` — stale page counts ("7 tap-to-cycle pages", `drawHomePage`/`drawOfflineScreen` naming, page-6/7 numbering); actual set is 5 pages both sides. Update to match code.
- `grok-report.md` — dated snapshot (2026-07-10); acceptable, optionally mark historical.
- `.gitignore` — add `.DS_Store` (currently only covered by user's global ignore).
- `pull_giphy_cats.py:171` — `--api-key` CLI arg leaks key to shell history/`ps`; prefer env/secrets file.
- `pull_giphy_cats.py:256` — query offsets grow past GIPHY's 4999 cap; mark query exhausted instead of "too many errors".
- `pull_giphy_cats.py:91–103` — existing-file check doesn't verify GIF magic; truncated files count toward target.
- `prepare_cat_gifs.py:111–120` — `tmp_co` file leaks on exception; try/finally cleanup.
- `drawLimitBlock` is dead code on both firmware and simulator — cleanup candidate.

## Verified OK (no action)

- Python 3.9.6 compat of both servers; thread exception survival; jsonl tailing (partial-line, truncation re-read); dedup on `(message.id, requestId)`; subprocess argv-only (no shell injection); OAuth token never logged/served; no path traversal; plists correct (labels, paths, KeepAlive semantics).
- Firmware mutex discipline elsewhere (only `sdMutex → stateMutex` nesting); cross-core String/buffer safety; all `snprintf`/`strncpy` bounded; AnimatedGIF callback safety; SD failure paths; NVS keys ≤15 chars; touch hit-boxes, confirm-arm, night mode, pixel shift, rotation; plain-HTTP-only as designed; full JSON contract parity with graceful defaults.
- All 13 RGB565→RGB888 color conversions exact; gfx emulation faithful (6px×size/char); page set is 5 on both sides; settings table field-for-field parity; server.html ↔ control_server.py API shapes consistent; logs rendered via `textContent` (safe).
- `secrets.local.json` and `config.h` untracked + gitignored; `cats/` intentional and ignored; no binaries/pycache tracked (26 files total); no other hardcoded secrets in tracked files.

## Suggested fix order

1. **Secrets**: rotate GIPHY key → env/`secrets.local.json`; gitignore `cats_raw/` + `.DS_Store`.
2. **Server security**: CSRF/Origin checks + scoped ACAO; HTTPS for WeatherAPI.
3. **Firmware correctness**: settings drag-state fix; SD-capacity cache off the render core; double-parse fix; `ctxTokens` int64; wrap-safe pacing; `pendingRestart`; AP HTML-escape; volatile flags.
4. **Server robustness**: EADDRINUSE guard, Content-Length cap, offsets pruning, naive-ts guard, log throttling, Open-Meteo tail fix, IncompleteRead catch (+ script nits).
5. **Simulator parity nits** + `server.html` null fix and innerHTML escaping.
6. **Docs**: refresh `CLAUDE.md` page counts, banner on `design.md`, drop dead `drawLimitBlock` both sides.
7. **Verify**: `/usr/bin/python3` parse check both servers; `arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=huge_app`; headless Playwright screenshot of simulator pages; `curl` the API contract.
