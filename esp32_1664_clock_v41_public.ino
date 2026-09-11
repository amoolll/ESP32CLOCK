/*
 * ESP32-S3 Internet Clock — 16x64 MAX7219, ground-up rewrite
 * ----------------------------------------------------------
 * VERSION: v34
 *
 * v34 — my verified v32 CORE (all stability work: aircraft backoff,
 * hardware watchdog, OTA-safe, connect timeouts, scroll-aware aircraft
 * timing, fetch-ahead) with a REDESIGNED WEB UI grafted on:
 *   - 7 focused cards (Network / Clock / Weather / Feeds / Aircraft /
 *     Appearance / System) instead of the old mixed layout.
 *   - Per-card save endpoints (/save/network, /save/clock, ...) that
 *     write only their own settings to NVS — most apply without a
 *     reboot (network change still reboots, as it must).
 *   - Live header: IP, firmware, uptime, and an RSSI signal bar that
 *     auto-refresh every 2s via a /system/status JSON endpoint.
 *   - System card: firmware / reset-reason / free-heap / RSSI stat
 *     grid, OTA + logs links, a "test pattern" button (all pixels on
 *     3s) and a confirm-gated factory reset (erases NVS + reboots).
 *   The core render/fetch/loop logic is byte-identical to v32 — only
 *   the web layer changed, plus the small test-pattern render hook.
 *
 * ============================================================================
 * VERSION: v32
 *
 * v32 — CLEAN REBUILD on the stable v23 base. The v24-v31 line had
 * accumulated instability (socket-exhaustion reboots, broken OTA, scroll
 * glitches), so this restarts from v23 (last known-good) and re-applies
 * only vetted features, in batches, validated between each:
 *
 *   BATCH 1 — stability:
 *     - Bounded connect timeouts on all fetches (setConnectTimeout).
 *     - Aircraft poll backoff: 2s -> 30s after 3 consecutive failures,
 *       snaps back on success. Fixes the socket-pool exhaustion that
 *       wedged the WiFi stack and caused RTC-watchdog reboots.
 *     - Aircraft "scrolls halfway then restarts" fixed: field duration
 *       is now scroll-aware (ends at one pass instead of letting the
 *       scroll wrap mid-phase). Display buffers only rewritten when the
 *       content changes, not every 2s poll.
 *     - News fetch-ahead moved to the START of the clock phase, so the
 *       fetch has the full window to finish and never blocks the scroll.
 *   BATCH 2 — watchdog:
 *     - Hardware Task WDT (15s), guarded for core 2.x/3.x. OTA-SAFE from
 *       the start: both web and IDE OTA pause the WDT during upload, so
 *       it never aborts a flash (the v30 OTA bug).
 *   BATCH 3 — content:
 *     - 16 reconciled news sources (Global/Europe/Asia/India sections),
 *       Moneycontrol (dead) replaced by LiveMint.
 *     - Dynamic news duration: each headline shows min(one scroll pass,
 *       newsCycleS) and scrolls exactly once.
 *   BATCH 4 — features:
 *     - Random feed order (shuffle-bag: each enabled feed once before
 *       repeats) vs Sequential, selectable in the News card.
 *     - News letter spacing (1 tight / 2 normal / 5 wide).
 *     - Auto day/night dimming with sunrise/sunset from Open-Meteo
 *       (parser FIXED — anchors to the daily block, sunset after
 *       sunrise, so rise != set).
 *     - Boot splash "LED Matrix Clock" / "to <wifi name>".
 *
 * ============================================================================
 * VERSION: v23
 *
 * NEW IN v23:
 *   - Split aircraft "Field Visible Duration" into two independent
 *     settings: "Show Carrier & Type for (s)" and "Show Departure &
 *     Arrival for (s)". Previously one shared value controlled both
 *     phases, so they always displayed for the same length of time.
 *   - Weather lat/lon now shown and saved with 4 decimal places
 *     (~11m precision) instead of 3 (~111m). The underlying storage
 *     was already a float with enough precision — only the display
 *     formatting was rounding it short. Inputs are now proper
 *     step=0.0001 number fields.
 *
 *
 * NEW IN v22 — reading old ESP8266 sketch and your logs closely:
 *   - Aircraft type was always blank ("-") because I was reading the
 *     wrong JSON field. The API returns the friendly name in
 *     "type_name" (as your old ESP8266 sketch does), not "type" or
 *     "aircraft_type" — those were guesses I made from other fields.
 *     Now checking type_name first, with the old keys as fallback.
 *   - Departure/Arrival was never showing because the session timeout
 *     was killing the display mid-phase-0. With acSess=10s and
 *     acFld=10s, phase 0 alone takes 11s (10s field + 1s blank), and
 *     the session ended before phase 1 (route info) ever started.
 *     Fixed: session timeout now only checked at phase transitions,
 *     and only after phase 1 has completed at least once (or been
 *     skipped for lack of route data). Guarantees you always see one
 *     complete carrier/type -> departure/arrival cycle per session,
 *     regardless of how short acSess is.
 *   - Dropped MarketWatch (persistent HTTP -1 connection failures in
 *     logs) and replaced CNBC World (persistent HTTP 503) with CNBC
 *     Top News which is more reliable. News source list is now 11
 *     sources; the mask indices shifted, so already-enabled sources
 *     might need re-checking after upgrade.
 *   - Fixed Moneycontrol (was returning HTTP 302 redirect that
 *     HTTPClient wasn't following). Enabled strict-follow-redirects
 *     for all news sources.
 *
 *
 * NEW IN v21 — performance pass + icon redesign:
 *   - Weather icons redesigned. The old rain animation moved drops
 *     UPWARD (wrong direction) and the cloud was a solid triangle
 *     blob. New set: proper cloud silhouette (bumpy top, rounded
 *     base), rain with 3 drops genuinely falling downward and
 *     wrapping, snow drifting down offset from rain, sun with pulsing
 *     rays, fog bands drifting sideways, storm with a lightning bolt
 *     that flashes on/off.
 *   - fbFlush() optimized: was testing each of 8 pixels per module
 *     individually (1024 64-bit shift+mask ops per frame); now
 *     extracts each module's 8-pixel byte in a single shift+mask
 *     (128 ops per frame).
 *   - Glyph and icon drawing rewritten to use a new fbOrRowBits()
 *     helper that ORs a whole row's bits into the frame buffer in one
 *     shift, instead of calling fbSetPixel() per pixel. A scrolling
 *     headline was doing ~350 fbSetPixel calls per frame; now ~70
 *     fbOrRowBits calls. Verified bit-identical to the old per-pixel
 *     path across edge cases (left/right clip, single pixel, overflow).
 *   - Dead code removed: unused SCROLL_STEP_MS constant and unused
 *     truncate() function (leftovers from earlier iterations).
 *   - fbSetPixel marked inline.
 *   These changes reduce per-frame CPU work substantially, which
 *   should help both the scroll smoothness and overall responsiveness
 *   (more loop headroom for server.handleClient() etc.).
 *
 *
 * NEW IN v20:
 *   - News scroll speed slider narrowed to a practical range. The old
 *     80ms(slow)-8ms(fast) span meant speeds 1-3 were unusably slow
 *     and 8-10 were an unreadable blur — nobody used the extremes.
 *     Now the full 1..10 slider maps across 52ms..31ms/column (was
 *     roughly old speeds 4-7), giving finer control within the range
 *     that's actually readable and usable.
 *   - Disabled MD_MAX72XX per-call auto-update: without this, every
 *     individual setRow() call (128 per frame — 16 devices x 8 rows)
 *     was triggering its own SPI transfer. Now setRow() only touches
 *     the in-memory buffer, and the single mx.update() at the end of
 *     fbFlush() pushes everything in one batch. Should reduce SPI
 *     overhead per frame — worth checking if this alone helps the
 *     jerkiness.
 *   - Added RenderStats logging (every 20s): render() call count and
 *     max render duration in microseconds, alongside the current
 *     scrollStepMs. This isolates whether the SPI flush itself is a
 *     source of frame-time variance, separate from network blocking
 *     (which the existing 60s Stats line already covers via loops/s
 *     and maxIterMs).
 *   - Confirmed news round-robin is already correct: nextEnabledSource()
 *     scans forward from the last index and wraps, so with all 12
 *     sources enabled it strictly cycles 1->2->...->12->1, never
 *     skipping or repeating early. With a 30s cycle (news shown every
 *     other slot, so effectively once per 60s), a full 12-source
 *     rotation takes 12 minutes before repeating.
 *
 *
 * NEW IN v19:
 *   - Real root cause of the scroll stutter (from your stats log
 *     showing maxIterMs spiking to 400-1200ms): v17/v18's scroll used
 *     an ACCUMULATING approach with a clamp on how many pixel-steps
 *     could catch up per frame. When a loop spike genuinely demanded
 *     20+ steps but the clamp capped it at 4, the scroll fell behind
 *     and STAYED behind — re-triggering the clamp on every subsequent
 *     frame near a fetch. That's a sustained stutter, not a one-time
 *     hiccup: clamping an accumulator creates permanent drift. Fixed
 *     by switching to ABSOLUTE-TIME positioning — scroll position is
 *     computed fresh every frame as (elapsed time since scroll start)
 *     / (ms per pixel), never accumulated from the previous position.
 *     A loop spike now just means one frame jumps a bit further than
 *     usual, then continues exactly on schedule — it cannot fall
 *     behind and stay behind, because there's nothing to catch up on.
 *   - Aircraft type no longer shows literal "Unknown Type" text —
 *     left blank (renders as "-") when the API doesn't return one.
 *   - Airline names shortened via a lookup table (KLM Royal Dutch
 *     Airlines -> KLM, Scandinavian Airlines System -> SAS, etc.) so
 *     they fit statically instead of scrolling constantly. Falls
 *     through unchanged for anything not in the table.
 *
 *
 * NEW IN v18:
 *   - Boot splash properly visible: "ESP32 Clock" + version for 2s,
 *     then "Connecting..." during WiFi. v17 would sometimes leap
 *     straight to an aircraft display within 6s of power-on, before
 *     the user could read the splash — because fetchAircraft ran
 *     immediately after WiFi came up. Not any more; splash is a
 *     guaranteed 2s minimum.
 *   - Scroll "fast then slow" bug fixed: when scroll transitioned
 *     from inactive to active, the catch-up mechanism from v17 would
 *     apply hundreds of pixel-steps in one frame (using the stale
 *     lastScrollStepMs from earlier in boot), then settle to the
 *     real rate. Fixed by resetting lastScrollStepMs on the rising
 *     edge of isScrolling. Also tightened the catch-up clamp from 32
 *     steps to 4 — makes brief loop stalls invisible instead of a
 *     visible fast-forward.
 *   - Aircraft display changed to 2-row layout matching the old
 *     ESP8266 sketches:
 *       Phase 0: Top row = Carrier   / Bottom row = Type
 *       Phase 1: Top row = Departure / Bottom row = Arrival
 *     Each row centered if short (<= aircraftMaxChars), scrolled
 *     independently if long. Phase 1 skipped if route info wasn't
 *     available.
 *   - Statistics logging: every 60s /logs gets a stats line with
 *     loops/second, longest single-iteration time, free heap, uptime.
 *     Loops/s should be thousands under normal conditions; drops
 *     below 100 indicate blocking calls hogging the loop. maxIterMs
 *     shows the worst-case block; if that's over 5000ms and the
 *     hang persists, we can identify which fetch is the culprit.
 *   - Software stall watchdog: if the loop somehow gets blocked for
 *     >30s (which is what "clock hangs after reboot, needs power
 *     cycle" often looks like), log it and restart cleanly instead
 *     of leaving the user to power cycle. Note: this only helps if
 *     the loop eventually RETURNS from whatever stalled it. A
 *     genuine hard-hang (SPI stall, silicon-level lock) needs a
 *     hardware WDT, which arduino-esp32 doesn't enable by default.
 *
 *
 * NEW IN v17:
 *   - News scroll speed slider actually works now. v16 had two hidden
 *     scroll bugs: (1) when the loop was blocked by an HTTPS fetch or
 *     web request, the scroll advanced by only ONE pixel when the loop
 *     resumed, no matter how much time had passed — capping effective
 *     scroll speed at whatever pace the loop's free time allowed
 *     (which is why the slider "did nothing"); (2) two independent
 *     timers for scroll advance and render could desync, causing
 *     micro-jitter. Fixed by computing elapsed/scrollStepMs and
 *     advancing all missed pixel steps in one go, then rendering only
 *     when the scroll actually moved. Single source of truth.
 *   - News pre-fetch: fetches happen 3s BEFORE the flip-in, not at
 *     flip-in. In v16, when a fetch took its full 5s timeout, the
 *     scroll froze for 5s right as the user was trying to read the
 *     headline. Now the (possibly slow) fetch happens during the last
 *     3s of the date-phase — invisible to the user — and by the flip
 *     the headline is already loaded and ready to scroll smoothly.
 *   - Boot log now dumps every persisted config value (news scroll
 *     speed, cycle, mask, aircraft timings, brightness) so /logs shows
 *     immediately what the sketch actually loaded from NVS. No more
 *     "did the slider persist?" mystery.
 *
 *
 * NEW IN v16:
 *   - Per-row brightness: MAX7219's INTENSITY register is per-chip, so
 *     with `mx.control(deviceIdx, INTENSITY, level)` we can actually
 *     set each 8x8 device's brightness independently. Now the clock
 *     brightness slider only affects the row hosting the clock, the
 *     news brightness only affects the row where news scrolls, etc.
 *     Rebuilds intensity on every render based on current activity.
 *   - Redesigned weather icons: 4-frame animations (instead of 2), new
 *     bitmaps that read as their subject at a glance. Rain drops now
 *     visibly fall through 4 vertical positions, sun rays pulse,
 *     lightning genuinely flashes on/off, clouds drift, snow drifts
 *     down. Cycled every 250ms (twice as fast).
 *   - Tightened HTTP timeouts (weather+news 5s, was default 60s) — a
 *     slow feed will now fail cleanly in 5s instead of blocking the
 *     main loop long enough to trigger a watchdog reset or freeze the
 *     web UI. IMPORTANT: this is only a partial fix for the "clock
 *     keeps rebooting" and "web page doesn't load" issues — please
 *     share /logs when a reboot happens so we can see the reset reason
 *     and heap size trend. Boot log now records reset reason and free
 *     heap, and each news fetch logs post-fetch heap so we can spot
 *     a memory leak if there is one.
 *
 *
 * NEW IN v15:
 *   - News sources rebuilt: dropped Reuters (RSS was killed in 2020),
 *     added 12 curated sources across US/India/Netherlands markets and
 *     general (Yahoo Finance, MarketWatch, CNBC, Moneycontrol, ET,
 *     DutchNews, NL Times, BBC, Al Jazeera, NDTV, TOI, The Hindu).
 *     Widened newsSourceMask from uint8_t to uint16_t (12 sources
 *     need >8 bits). Struct-based per-source display tag now instead
 *     of a hand-coded switch, so tags stay in sync with source list.
 *   - News scroll speed slider (1-10) in web UI, mapped linearly to
 *     80-8ms/column.
 *   - Moderate headline sanitization: " and " -> " & ", strip stray
 *     double quotes and Unicode smart quotes, collapse double spaces.
 *     Deliberately keeps "?" — question marks carry meaning; stripping
 *     them makes questions read as statements.
 *   - Aircraft display rebuilt per the old ESP8266 sketch semantics:
 *     * Detail Session Duration (0 = show while aircraft is present)
 *     * Field Visible Duration per field (Carrier / Type / From / To)
 *     * Blank Gap between fields
 *     * Scroll Speed (ms/column)
 *     * Max Chars before scroll (shorter text stays static+centered)
 *     Fields rotate through Carrier -> blank -> Type -> blank ->
 *     From -> blank -> To -> blank -> loop. Route fields are skipped
 *     if origin/destination weren't returned by the /routes lookup.
 *     Aircraft display overrides clock+news+date completely while
 *     active, and returns to the clock after the session times out
 *     even if the plane is still overhead.
 *   - Per-task brightness: clock, news, and aircraft each get their
 *     own 0..15 brightness setting, applied automatically as the
 *     active screen state changes. Prevents "news at 15 lights up
 *     the whole room at night" style annoyances.
 *
 * VERSION: v14
 *
 * NEW IN v14:
 *   - Icon animation actually works: v13's render trigger only fired at
 *     1Hz for static content, missing the 500ms icon frame toggles.
 *     Now render also fires whenever the icon frame flips, on top of the
 *     scroll and 1s ticks.
 *   - PM/AM spacing reduced from 2 spaces to 1 (was overshifted).
 *   - Aircraft display expanded to full 2-row takeover: while a plane is
 *     overhead, both rows are aircraft info. Sub-phase alternates every
 *     10s between (Carrier | Type) and (From city | To city). Route info
 *     only shown if the /routes/<callsign> lookup succeeded — never
 *     shows "Unknown" flashing on a network hiccup. Text that fits in
 *     64px stays centered/static; longer text scrolls.
 *   - Aircraft always overrides news+date (per your priority spec).
 *
 *
 * DESIGN CHANGE FROM v1-v11:
 *   Previous versions used MD_Parola for text rendering, which was
 *   fighting the hardware's mirrored device layout at multiple levels
 *   (zone offsets, PA_PRINT timing, non-idempotent post-render swaps,
 *   racing timers). Every fix opened a new bug. This rewrite drops
 *   Parola entirely and takes full control:
 *
 *   1. Own 16-row x 64-column pixel buffer (frameBuffer[16], one uint64
 *      per row where each bit is a pixel).
 *   2. Own 5x7 font, drawn character by character straight into the
 *      buffer at arbitrary (x,y) positions.
 *   3. Explicit flush: reads the buffer, writes each 8-pixel column to
 *      the correct physical module using the known device layout
 *      (device 15 = top-left, device 0 = bottom-right, etc.) via
 *      mx->setRow(). Same primitive already proven reliable for the
 *      weather icon.
 *
 *   Result: no library timing to fight. Whatever's in frameBuffer[] is
 *   exactly what shows on the display, deterministically.
 *
 * HARDWARE (unchanged): single 16-device MAX7219 chain, DIN=GPIO4,
 * CLK=GPIO8, CS=GPIO5.
 *
 * CONFIRMED HARDWARE LAYOUT (from corner ID test in earlier session):
 *   Physical row TOP    = devices 15..8 (device15=leftmost, 8=rightmost)
 *   Physical row BOTTOM = devices  7..0 (device7=leftmost, 0=rightmost)
 *   Within each module, row 0 is at the top, column 0 is at the left.
 *
 * SCOPE FOR THIS VERSION:
 *   - Static clock + date, weather icon, web UI, OTA, brightness — all in.
 *   - News RSS fetch — kept in code but ticker rendering deferred: the
 *     first job here is confirming static text works cleanly. Once that's
 *     verified in your photo, next iteration adds a proper hand-rolled
 *     scroll routine over this same buffer.
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include <MD_MAX72xx.h>
#include <SPI.h>
#include <Preferences.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <WebServer.h>
#include <esp_task_wdt.h>

#define FIRMWARE_VERSION "v41"

// ---------- WEATHER ICON ENUM (defined here — Arduino auto-prototypes need it early) ----------
enum WeatherIcon { W_SUNNY, W_CLOUDY, W_RAINY, W_SNOWY, W_FOGGY, W_STORMY, W_NONE };

// ---------- DEFAULT FALLBACK CREDENTIALS ----------
#define DEFAULT_WIFI_SSID     "YOUR_WIFI_SSID"
#define DEFAULT_WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// ---------- NTP ----------
const char* NTP_SERVER_1 = "pool.ntp.org";
const char* NTP_SERVER_2 = "time.nist.gov";

// ---------- DEFAULTS ----------
#define DEFAULT_USE_24H       false
#define DEFAULT_BRIGHTNESS    4
#define DEFAULT_CITY_INDEX    0
#define DEFAULT_WEATHER_LAT   0.0f   // set your own latitude via the web UI
#define DEFAULT_WEATHER_LON   0.0f   // set your own longitude via the web UI

// ---------- CITY LIST ----------
struct CityTZ { const char* name; const char* posixTZ; };
const CityTZ CITY_LIST[] = {
  { "Amsterdam", "CET-1CEST,M3.5.0,M10.5.0/3" },
  { "London",    "GMT0BST,M3.5.0/1,M10.5.0" },
  { "New York",  "EST5EDT,M3.2.0,M11.1.0" },
  { "Mumbai",    "IST-5:30" },
  { "Dubai",     "<+04>-4" },
  { "Tokyo",     "JST-9" },
  { "Sydney",    "AEST-10AEDT,M10.1.0,M4.1.0/3" },
};
const uint8_t CITY_COUNT = sizeof(CITY_LIST) / sizeof(CITY_LIST[0]);

// ---------- MATRIX CONFIG ----------
#define HARDWARE_TYPE MD_MAX72XX::FC16_HW
#define MAX_DEVICES   16
#define DATA_PIN  4
#define CLK_PIN   8
#define CS_PIN    5

MD_MAX72XX mx = MD_MAX72XX(HARDWARE_TYPE, DATA_PIN, CLK_PIN, CS_PIN, MAX_DEVICES);

// ============================================================================
// FRAME BUFFER — 16 rows x 64 cols, our source of truth for the display
// ============================================================================
// Each row is a 64-bit value; bit 63 = leftmost pixel, bit 0 = rightmost.
// Row 0 = top, row 15 = bottom. That's the natural reading orientation;
// the flush routine handles remapping to the physical module layout.
uint64_t frameBuffer[16];

void fbClear() {
  for (uint8_t r = 0; r < 16; r++) frameBuffer[r] = 0;
}

inline void fbSetPixel(int x, int y, bool on) {
  if (x < 0 || x > 63 || y < 0 || y > 15) return;
  uint64_t bit = (uint64_t)1 << (63 - x);
  if (on) frameBuffer[y] |= bit;
  else    frameBuffer[y] &= ~bit;
}

// Fast path for drawing a horizontal run of up to 8 pixels (one glyph
// row) into the frame buffer in a single shift+OR, instead of calling
// fbSetPixel per pixel. `bits` is right-aligned (bit (width-1) =
// leftmost pixel). x is the frame column of the leftmost pixel.
// Positions the pattern with ONE shift and ORs it in one operation;
// off-screen bits naturally fall outside the 64-bit word and vanish.
inline void fbOrRowBits(int x, int y, uint32_t bits, uint8_t width) {
  if (y < 0 || y > 15 || bits == 0) return;
  // Leftmost pattern pixel goes to frame column x = bit (63 - x).
  // The pattern's MSB (bit width-1) is its leftmost pixel, so the whole
  // pattern's target position for bit (width-1) is (63 - x). Shift the
  // pattern so bit (width-1) lands at (63 - x):
  //   targetShift = (63 - x) - (width - 1) = 64 - x - width
  int targetShift = 64 - x - (int)width;
  uint64_t pattern = (uint64_t)(bits & ((1u << width) - 1));
  if (targetShift >= 0 && targetShift < 64) {
    frameBuffer[y] |= (pattern << targetShift);
  } else if (targetShift <= -64 || targetShift >= 64) {
    return;   // fully off-screen
  } else if (targetShift < 0) {
    frameBuffer[y] |= (pattern >> (-targetShift));   // clipped on the right edge
  }
}

// ============================================================================
// FLUSH — write frameBuffer to the physical modules
// ============================================================================
// Physical layout, confirmed via corner ID test:
//   TOP ROW    = modules 15..8, device15 is leftmost (columns 0..7),
//                                device8 is rightmost (columns 56..63).
//                Frame rows 0..7 map here.
//   BOTTOM ROW = modules 7..0,  device7 is leftmost (columns 0..7),
//                                device0 is rightmost (columns 56..63).
//                Frame rows 8..15 map here.
// For each device, we send 8 rows of 8 pixels using setRow(dev, row, byte).
// setRow's byte format: bit 7 = leftmost pixel of that module.
void fbFlush() {
  // Performance-critical: called every frame. Previous version tested
  // each of the 8 pixels in each module individually (a 64-bit shift +
  // mask per pixel = 1024 such ops per frame). This version extracts
  // the whole 8-pixel byte for a module in one shift+mask.
  //
  // frameBuffer[row] has bit 63 = leftmost pixel (frame col 0). Module
  // `moduleCol` covers frame cols moduleCol*8 .. moduleCol*8+7. To get
  // those 8 bits right-aligned into a byte: shift the row right so that
  // the module's leftmost pixel lands in bit 7, then mask to 8 bits.
  //   shift amount = 63 - (moduleCol*8) - 7 = 56 - moduleCol*8
  for (uint8_t moduleCol = 0; moduleCol < 8; moduleCol++) {
    uint8_t topDevice    = 15 - moduleCol;
    uint8_t bottomDevice = 7  - moduleCol;
    uint8_t shift = 56 - (moduleCol * 8);
    for (uint8_t innerRow = 0; innerRow < 8; innerRow++) {
      uint8_t topByte    = (uint8_t)((frameBuffer[innerRow]     >> shift) & 0xFF);
      uint8_t bottomByte = (uint8_t)((frameBuffer[8 + innerRow] >> shift) & 0xFF);
      mx.setRow(topDevice,    innerRow, topByte);
      mx.setRow(bottomDevice, innerRow, bottomByte);
    }
  }
  mx.update();
}

// ============================================================================
// FONT — 5x7 pixels per character, monospaced (drawn in a 6px-wide slot with 1px spacing)
// ============================================================================
// Each character is 7 rows (top to bottom), each row is 5 bits (bit 4 = leftmost).
// Only ASCII 0x20 (space) through 0x7A (z) are defined; anything else prints as space.
struct Glyph { uint8_t rows[7]; };

const Glyph FONT[96] = {
  // 0x20 space
  {{0,0,0,0,0,0,0}},
  // 0x21 !
  {{0x04,0x04,0x04,0x04,0x00,0x04,0x00}},
  // 0x22 "
  {{0x0A,0x0A,0x00,0x00,0x00,0x00,0x00}},
  // 0x23 #
  {{0x0A,0x1F,0x0A,0x0A,0x1F,0x0A,0x00}},
  // 0x24 $
  {{0x04,0x1E,0x05,0x0E,0x14,0x0F,0x04}},
  // 0x25 %
  {{0x19,0x19,0x02,0x04,0x08,0x13,0x13}},
  // 0x26 &
  {{0x06,0x09,0x05,0x02,0x15,0x09,0x16}},
  // 0x27 '
  {{0x04,0x04,0x00,0x00,0x00,0x00,0x00}},
  // 0x28 (
  {{0x02,0x04,0x08,0x08,0x08,0x04,0x02}},
  // 0x29 )
  {{0x08,0x04,0x02,0x02,0x02,0x04,0x08}},
  // 0x2A *
  {{0x00,0x0A,0x04,0x1F,0x04,0x0A,0x00}},
  // 0x2B +
  {{0x00,0x04,0x04,0x1F,0x04,0x04,0x00}},
  // 0x2C ,
  {{0x00,0x00,0x00,0x00,0x00,0x04,0x08}},
  // 0x2D -
  {{0x00,0x00,0x00,0x1F,0x00,0x00,0x00}},
  // 0x2E .
  {{0x00,0x00,0x00,0x00,0x00,0x00,0x04}},
  // 0x2F /
  {{0x00,0x01,0x02,0x04,0x08,0x10,0x00}},
  // 0x30 0
  {{0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}},
  // 0x31 1
  {{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}},
  // 0x32 2
  {{0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}},
  // 0x33 3
  {{0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}},
  // 0x34 4
  {{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}},
  // 0x35 5
  {{0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}},
  // 0x36 6
  {{0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}},
  // 0x37 7
  {{0x1F,0x01,0x02,0x04,0x08,0x08,0x08}},
  // 0x38 8
  {{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}},
  // 0x39 9
  {{0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}},
  // 0x3A :
  {{0x00,0x04,0x00,0x00,0x00,0x04,0x00}},
  // 0x3B ;
  {{0x00,0x04,0x00,0x00,0x00,0x04,0x08}},
  // 0x3C <
  {{0x02,0x04,0x08,0x10,0x08,0x04,0x02}},
  // 0x3D =
  {{0x00,0x00,0x1F,0x00,0x1F,0x00,0x00}},
  // 0x3E >
  {{0x08,0x04,0x02,0x01,0x02,0x04,0x08}},
  // 0x3F ?
  {{0x0E,0x11,0x01,0x02,0x04,0x00,0x04}},
  // 0x40 @
  {{0x0E,0x11,0x17,0x15,0x17,0x10,0x0E}},
  // 0x41 A
  {{0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}},
  // 0x42 B
  {{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}},
  // 0x43 C
  {{0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}},
  // 0x44 D
  {{0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}},
  // 0x45 E
  {{0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}},
  // 0x46 F
  {{0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}},
  // 0x47 G
  {{0x0E,0x11,0x10,0x17,0x11,0x11,0x0E}},
  // 0x48 H
  {{0x11,0x11,0x11,0x1F,0x11,0x11,0x11}},
  // 0x49 I
  {{0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}},
  // 0x4A J
  {{0x07,0x02,0x02,0x02,0x02,0x12,0x0C}},
  // 0x4B K
  {{0x11,0x12,0x14,0x18,0x14,0x12,0x11}},
  // 0x4C L
  {{0x10,0x10,0x10,0x10,0x10,0x10,0x1F}},
  // 0x4D M
  {{0x11,0x1B,0x15,0x15,0x11,0x11,0x11}},
  // 0x4E N
  {{0x11,0x11,0x19,0x15,0x13,0x11,0x11}},
  // 0x4F O
  {{0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}},
  // 0x50 P
  {{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}},
  // 0x51 Q
  {{0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}},
  // 0x52 R
  {{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}},
  // 0x53 S
  {{0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}},
  // 0x54 T
  {{0x1F,0x04,0x04,0x04,0x04,0x04,0x04}},
  // 0x55 U
  {{0x11,0x11,0x11,0x11,0x11,0x11,0x0E}},
  // 0x56 V
  {{0x11,0x11,0x11,0x11,0x11,0x0A,0x04}},
  // 0x57 W
  {{0x11,0x11,0x11,0x15,0x15,0x15,0x0A}},
  // 0x58 X
  {{0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}},
  // 0x59 Y
  {{0x11,0x11,0x11,0x0A,0x04,0x04,0x04}},
  // 0x5A Z
  {{0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}},
  // 0x5B [
  {{0x0E,0x08,0x08,0x08,0x08,0x08,0x0E}},
  // 0x5C backslash
  {{0x00,0x10,0x08,0x04,0x02,0x01,0x00}},
  // 0x5D ]
  {{0x0E,0x02,0x02,0x02,0x02,0x02,0x0E}},
  // 0x5E ^
  {{0x04,0x0A,0x11,0x00,0x00,0x00,0x00}},
  // 0x5F _
  {{0x00,0x00,0x00,0x00,0x00,0x00,0x1F}},
  // 0x60 `
  {{0x08,0x04,0x00,0x00,0x00,0x00,0x00}},
  // 0x61 a
  {{0x00,0x00,0x0E,0x01,0x0F,0x11,0x0F}},
  // 0x62 b
  {{0x10,0x10,0x16,0x19,0x11,0x11,0x1E}},
  // 0x63 c
  {{0x00,0x00,0x0E,0x10,0x10,0x11,0x0E}},
  // 0x64 d
  {{0x01,0x01,0x0D,0x13,0x11,0x11,0x0F}},
  // 0x65 e
  {{0x00,0x00,0x0E,0x11,0x1F,0x10,0x0E}},
  // 0x66 f
  {{0x06,0x09,0x08,0x1C,0x08,0x08,0x08}},
  // 0x67 g
  {{0x00,0x0F,0x11,0x11,0x0F,0x01,0x0E}},
  // 0x68 h
  {{0x10,0x10,0x16,0x19,0x11,0x11,0x11}},
  // 0x69 i
  {{0x04,0x00,0x0C,0x04,0x04,0x04,0x0E}},
  // 0x6A j
  {{0x02,0x00,0x06,0x02,0x02,0x12,0x0C}},
  // 0x6B k
  {{0x10,0x10,0x12,0x14,0x18,0x14,0x12}},
  // 0x6C l
  {{0x0C,0x04,0x04,0x04,0x04,0x04,0x0E}},
  // 0x6D m
  {{0x00,0x00,0x1A,0x15,0x15,0x11,0x11}},
  // 0x6E n
  {{0x00,0x00,0x16,0x19,0x11,0x11,0x11}},
  // 0x6F o
  {{0x00,0x00,0x0E,0x11,0x11,0x11,0x0E}},
  // 0x70 p
  {{0x00,0x00,0x1E,0x11,0x1E,0x10,0x10}},
  // 0x71 q
  {{0x00,0x00,0x0D,0x13,0x0F,0x01,0x01}},
  // 0x72 r
  {{0x00,0x00,0x16,0x19,0x10,0x10,0x10}},
  // 0x73 s
  {{0x00,0x00,0x0F,0x10,0x0E,0x01,0x1E}},
  // 0x74 t
  {{0x08,0x08,0x1C,0x08,0x08,0x09,0x06}},
  // 0x75 u
  {{0x00,0x00,0x11,0x11,0x11,0x13,0x0D}},
  // 0x76 v
  {{0x00,0x00,0x11,0x11,0x11,0x0A,0x04}},
  // 0x77 w
  {{0x00,0x00,0x11,0x11,0x15,0x15,0x0A}},
  // 0x78 x
  {{0x00,0x00,0x11,0x0A,0x04,0x0A,0x11}},
  // 0x79 y
  {{0x00,0x00,0x11,0x11,0x0F,0x01,0x0E}},
  // 0x7A z
  {{0x00,0x00,0x1F,0x02,0x04,0x08,0x1F}},
  // 0x7B..0x7F — space (unused)
  {{0,0,0,0,0,0,0}},{{0,0,0,0,0,0,0}},{{0,0,0,0,0,0,0}},{{0,0,0,0,0,0,0}},{{0,0,0,0,0,0,0}},
};

// Draw one character at (x, y) — (x,y) is the top-left of a 5x7 glyph.
// Character width is 5 pixels; leave a 1px gap for spacing between chars.
void drawChar(int x, int y, char c) {
  if (c < 0x20 || c > 0x7F) c = 0x20;
  const Glyph& g = FONT[c - 0x20];
  for (uint8_t row = 0; row < 7; row++) {
    // Each glyph row is 5 bits, already right-aligned with bit 4 =
    // leftmost pixel — exactly what fbOrRowBits wants.
    fbOrRowBits(x, y + row, g.rows[row], 5);
  }
}

// Total pixel width of `s` if drawn with drawString (5px char + 1px space each, except no trailing space).
int stringWidthPx(const char* s) {
  int len = strlen(s);
  if (len == 0) return 0;
  return len * 6 - 1;
}

// Width when each char advances by (6 + extraSpacing), clamped so the
// advance never drops below the 5px glyph width (which would overlap).
int stringWidthPxSpaced(const char* s, int extraSpacing) {
  int len = strlen(s);
  if (len == 0) return 0;
  int advance = 6 + extraSpacing;
  if (advance < 5) advance = 5;
  return len * advance - 1;
}

void drawString(int x, int y, const char* s) {
  while (*s) {
    drawChar(x, y, *s);
    x += 6;   // 5px glyph + 1px space
    s++;
  }
}

void drawStringCentered(int leftEdge, int width, int y, const char* s) {
  int w = stringWidthPx(s);
  int x = leftEdge + (width - w) / 2;
  drawString(x, y, s);
}

// Draw a scrolling string across a clip window [leftEdge..leftEdge+width-1].
// extraSpacing adds/removes px between chars (0 = default). scrollX is the
// current pixel offset. Pixels outside the clip window are not drawn.
void drawScrollingString(int leftEdge, int width, int y, const char* s, int scrollX, int extraSpacing = 0) {
  int advance = 6 + extraSpacing;
  if (advance < 5) advance = 5;
  int w = stringWidthPxSpaced(s, extraSpacing);
  if (w == 0) return;
  int totalRange = w + width;   // string enters, crosses, and fully exits
  int effectiveScroll = scrollX % totalRange;
  int xStart = leftEdge + width - effectiveScroll;
  int cursor = xStart;
  for (const char* p = s; *p; p++) {
    if (cursor + 5 > leftEdge && cursor < leftEdge + width) {
      char c = *p;
      if (c < 0x20 || c > 0x7F) c = 0x20;
      const Glyph& g = FONT[c - 0x20];
      for (uint8_t row = 0; row < 7; row++) {
        uint8_t bits = g.rows[row];
        if (bits == 0) continue;
        for (uint8_t col = 0; col < 5; col++) {
          int px = cursor + col;
          if (px < leftEdge || px >= leftEdge + width) {
            bits &= ~(1 << (4 - col));
          }
        }
        fbOrRowBits(cursor, y + row, bits, 5);
      }
    }
    cursor += advance;
  }
}

// ============================================================================
// WEATHER ICONS — 8x8 bitmaps drawn straight into the frame buffer
// ============================================================================
// Weather icons: 4-frame animation, cycled every ~250ms. Each icon is
// 8x8, where row bit 7 = leftmost pixel. Designed for visible motion at
// small scale:
//   Sunny: rays pulse in/out (small -> medium -> large -> medium)
//   Cloudy: cloud drifts left-right slightly (2 positions, over 4 frames)
//   Rainy: sun-and-cloud with 4-position falling drops (drops appear
//     to move down each frame)
//   Snowy: snowflakes at different positions (drifting downward)
//   Foggy: fog bands drift horizontally (each frame shifts 1 pixel)
//   Stormy: dark cloud (fixed) + lightning bolt flashes (bolt visible
//     only on 1 of 4 frames — creates a real "flash" effect)
//   Windy (used for high-wind weather codes): moving wind lines
//   Thunder-in-storm: sharp bolt every other frame

// Weather icons: 4-frame animations, cycled every ~250ms. Redesigned in
// v21 to actually read as their subject at a glance:
//   SUNNY  - sun disc with rays that pulse out/in
//   CLOUDY - a proper cloud silhouette (bumpy top, rounded base) that
//            drifts left and right
//   RAINY  - cloud with 3 raindrops that genuinely fall DOWNWARD (each
//            frame the drops descend one row and wrap — earlier version
//            moved them up by mistake)
//   SNOWY  - cloud with snowflakes drifting down, slightly slower/offset
//            from rain so they read differently
//   FOGGY  - horizontal fog bands drifting sideways
//   STORMY - cloud with a lightning bolt that flashes ON/off/ON/off
const uint8_t ICON_SUNNY_F0[8]  = {0b00000000, 0b00011000, 0b00111100, 0b01111110, 0b01111110, 0b00111100, 0b00011000, 0b00000000};
const uint8_t ICON_SUNNY_F1[8]  = {0b00011000, 0b00011000, 0b00111100, 0b11111111, 0b11111111, 0b00111100, 0b00011000, 0b00011000};
const uint8_t ICON_SUNNY_F2[8]  = {0b10011001, 0b01011010, 0b00111100, 0b01111110, 0b01111110, 0b00111100, 0b01011010, 0b10011001};
const uint8_t ICON_SUNNY_F3[8]  = {0b00011000, 0b00011000, 0b00111100, 0b11111111, 0b11111111, 0b00111100, 0b00011000, 0b00011000};
const uint8_t ICON_CLOUDY_F0[8] = {0b00000000, 0b00001100, 0b00011110, 0b01111111, 0b11111111, 0b11111111, 0b01111110, 0b00000000};
const uint8_t ICON_CLOUDY_F1[8] = {0b00000000, 0b00000110, 0b00001111, 0b00111111, 0b01111111, 0b01111111, 0b00111111, 0b00000000};
const uint8_t ICON_CLOUDY_F2[8] = {0b00000000, 0b00001100, 0b00011110, 0b01111111, 0b11111111, 0b11111111, 0b01111110, 0b00000000};
const uint8_t ICON_CLOUDY_F3[8] = {0b00000000, 0b00011000, 0b00111100, 0b11111110, 0b11111110, 0b11111110, 0b11111100, 0b00000000};
const uint8_t ICON_RAINY_F0[8]  = {0b00001100, 0b00011110, 0b01111111, 0b11111111, 0b01000000, 0b00000010, 0b00001000, 0b00000000};
const uint8_t ICON_RAINY_F1[8]  = {0b00001100, 0b00011110, 0b01111111, 0b11111111, 0b00000000, 0b01000000, 0b00000010, 0b00001000};
const uint8_t ICON_RAINY_F2[8]  = {0b00001100, 0b00011110, 0b01111111, 0b11111111, 0b00001000, 0b00000000, 0b01000000, 0b00000010};
const uint8_t ICON_RAINY_F3[8]  = {0b00001100, 0b00011110, 0b01111111, 0b11111111, 0b00000010, 0b00001000, 0b00000000, 0b01000000};
const uint8_t ICON_SNOWY_F0[8]  = {0b00001100, 0b00011110, 0b01111111, 0b11111111, 0b00100000, 0b01000000, 0b00000100, 0b00000000};
const uint8_t ICON_SNOWY_F1[8]  = {0b00001100, 0b00011110, 0b01111111, 0b11111111, 0b00000000, 0b00100000, 0b01000000, 0b00000100};
const uint8_t ICON_SNOWY_F2[8]  = {0b00001100, 0b00011110, 0b01111111, 0b11111111, 0b00000100, 0b00000000, 0b00100000, 0b01000000};
const uint8_t ICON_SNOWY_F3[8]  = {0b00001100, 0b00011110, 0b01111111, 0b11111111, 0b01000000, 0b00000100, 0b00000000, 0b00100000};
const uint8_t ICON_FOGGY_F0[8]  = {0b00000000, 0b11111100, 0b00000000, 0b11111110, 0b00000000, 0b11111100, 0b00000000, 0b11111110};
const uint8_t ICON_FOGGY_F1[8]  = {0b00000000, 0b01111110, 0b00000000, 0b11111111, 0b00000000, 0b01111110, 0b00000000, 0b11111111};
const uint8_t ICON_FOGGY_F2[8]  = {0b00000000, 0b00111111, 0b00000000, 0b01111111, 0b00000000, 0b00111111, 0b00000000, 0b01111111};
const uint8_t ICON_FOGGY_F3[8]  = {0b00000000, 0b00011111, 0b00000000, 0b00111111, 0b00000000, 0b00011111, 0b00000000, 0b00111111};
const uint8_t ICON_STORMY_F0[8] = {0b00001100, 0b00011110, 0b01111111, 0b11111111, 0b00011000, 0b00110000, 0b01111000, 0b00110000};
const uint8_t ICON_STORMY_F1[8] = {0b00001100, 0b00011110, 0b01111111, 0b11111111, 0b00000000, 0b00000000, 0b00000000, 0b00000000};
const uint8_t ICON_STORMY_F2[8] = {0b00001100, 0b00011110, 0b01111111, 0b11111111, 0b00011000, 0b00110000, 0b01111000, 0b00110000};
const uint8_t ICON_STORMY_F3[8] = {0b00001100, 0b00011110, 0b01111111, 0b11111111, 0b00000000, 0b00000000, 0b00000000, 0b00000000};

uint8_t iconFrame = 0;   // 0..3, cycled every ~250ms in loop()

void drawWeatherIconAt(int x, int y, WeatherIcon w) {
  const uint8_t* frames[4] = { nullptr, nullptr, nullptr, nullptr };
  switch (w) {
    case W_SUNNY:  frames[0]=ICON_SUNNY_F0;  frames[1]=ICON_SUNNY_F1;  frames[2]=ICON_SUNNY_F2;  frames[3]=ICON_SUNNY_F3;  break;
    case W_CLOUDY: frames[0]=ICON_CLOUDY_F0; frames[1]=ICON_CLOUDY_F1; frames[2]=ICON_CLOUDY_F2; frames[3]=ICON_CLOUDY_F3; break;
    case W_RAINY:  frames[0]=ICON_RAINY_F0;  frames[1]=ICON_RAINY_F1;  frames[2]=ICON_RAINY_F2;  frames[3]=ICON_RAINY_F3;  break;
    case W_SNOWY:  frames[0]=ICON_SNOWY_F0;  frames[1]=ICON_SNOWY_F1;  frames[2]=ICON_SNOWY_F2;  frames[3]=ICON_SNOWY_F3;  break;
    case W_FOGGY:  frames[0]=ICON_FOGGY_F0;  frames[1]=ICON_FOGGY_F1;  frames[2]=ICON_FOGGY_F2;  frames[3]=ICON_FOGGY_F3;  break;
    case W_STORMY: frames[0]=ICON_STORMY_F0; frames[1]=ICON_STORMY_F1; frames[2]=ICON_STORMY_F2; frames[3]=ICON_STORMY_F3; break;
    default: return;
  }
  const uint8_t* bmp = frames[iconFrame & 3];
  for (uint8_t row = 0; row < 8; row++) {
    // Icon rows are 8 bits, bit 7 = leftmost — feed straight to fbOrRowBits.
    fbOrRowBits(x, y + row, bmp[row], 8);
  }
}

WeatherIcon mapWeatherCode(int code) {
  if (code == 0) return W_SUNNY;
  if (code >= 1 && code <= 3) return W_CLOUDY;
  if (code == 45 || code == 48) return W_FOGGY;
  if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) return W_RAINY;
  if ((code >= 71 && code <= 77) || code == 85 || code == 86) return W_SNOWY;
  if (code >= 95) return W_STORMY;
  return W_CLOUDY;
}

// ---------- NEWS SOURCES ----------
struct NewsSource { const char* name; const char* tag; const char* url; };
const NewsSource NEWS_SOURCES[] = {
  // --- Global & Business ---
  { "BBC News - World",           "BBC",        "https://feeds.bbci.co.uk/news/world/rss.xml" },
  { "Al Jazeera - All",           "Al Jazeera", "https://www.aljazeera.com/xml/rss/all.xml" },
  { "The Guardian - World",       "Guardian",   "https://www.theguardian.com/world/rss" },
  { "CNBC Top News",              "CNBC",       "https://www.cnbc.com/id/100003114/device/rss/rss.html" },
  { "Yahoo Finance",              "Yahoo",      "https://finance.yahoo.com/news/rssindex" },
  // --- Europe & Local ---
  { "Deutsche Welle",             "DW",         "https://rss.dw.com/rdf/rss-en-top" },
  { "France 24 - English",        "France24",   "https://www.france24.com/en/rss" },
  { "Euronews",                   "Euronews",   "https://www.euronews.com/rss" },
  { "DutchNews.nl",               "DutchNews",  "https://www.dutchnews.nl/feed/" },
  // --- Asia (Japan & China) ---
  { "Japan Times",                "JapanTimes", "https://www.japantimes.co.jp/feed/" },
  { "South China Morning Post",   "SCMP",       "https://www.scmp.com/rss/5/feed" },
  // --- India ---
  { "Indian Express",             "IE",         "https://indianexpress.com/section/india/feed/" },
  { "The Hindu - National",       "The Hindu",  "https://www.thehindu.com/news/national/feeder/default.rss" },
  { "NDTV Top Stories",           "NDTV",       "https://feeds.feedburner.com/ndtvnews-top-stories" },
  { "LiveMint - Markets",         "LiveMint",   "https://www.livemint.com/rss/markets" },
  { "Times of India - Top",       "TOI",        "https://timesofindia.indiatimes.com/rssfeedstopstories.cms" },
};
const uint8_t NEWS_SOURCE_COUNT = sizeof(NEWS_SOURCES) / sizeof(NEWS_SOURCES[0]);

// ---------- STATE + PREFS ----------
Preferences prefs;
String wifiSsid, wifiPassword;
bool use24h;
uint8_t brightness, cityIndex;
bool clockOnTop;
float weatherLat, weatherLon;

// News
bool newsEnabled;
uint16_t newsSourceMask;
uint16_t newsCycleS;
uint8_t newsScrollSpeed;   // 1 (slow) to 10 (fast), maps to a ms-per-column value
uint8_t newsRotationIdx = 0;
uint8_t newsOrderMode = 0;        // 0 = sequential, 1 = random (shuffle bag)
uint16_t newsShownMask = 0;       // shuffle-bag: sources already shown this cycle
uint8_t newsLetterSpacing = 2;    // 1 tight, 2 normal, 3-5 wider (news scroll only)
char newsHeadline[160] = "";
bool newsAvailable = false;
bool showingNews = false;
unsigned long lastDateNewsFlipMs = 0;

// Aircraft
bool aircraftEnabled;
String aircraftApiHost;
uint16_t aircraftApiPort;
// Aircraft display timings (all web-configurable):
//   SessionSec    = total cap on how long the aircraft takeover lasts
//                   before returning to the clock, even if the plane
//                   is still overhead. 0 = no cap (show while present).
//                   Checked only at phase transitions (see loop()), so
//                   it never cuts a phase off mid-display — you always
//                   see one complete Carrier/Type -> Departure/Arrival
//                   cycle before it can end the session.
//   FieldSec      = how long PHASE 0 (Carrier + Type) is shown.
//   FieldSec2     = how long PHASE 1 (Departure + Arrival) is shown.
//                   Separate from FieldSec so you can e.g. show the
//                   carrier/type briefly but linger longer on the
//                   route, or vice versa.
//   BlankMs       = blank pause between phases, for visual separation.
//   ScrollMs      = ms per column for scrolling text that's too long
//                   to fit statically (lower = faster).
//   MaxChars      = text at or under this length is centered and
//                   static; longer text scrolls.
uint16_t aircraftSessionSec;
uint16_t aircraftFieldSec;    // Phase 0 (Carrier/Type) duration
uint16_t aircraftFieldSec2;   // Phase 1 (Departure/Arrival) duration
uint16_t aircraftBlankMs;
uint8_t aircraftScrollMs;
uint8_t aircraftMaxChars;

// Per-task brightness: each screen state can dim/brighten independently
// so news doesn't blast at full brightness or the clock isn't too bright
// at night when the aircraft display is off. Values 0..15.
uint8_t brightnessClock;
uint8_t brightnessNews;
uint8_t brightnessAircraft;

// Day/Night auto-dimming: when on, the per-task brightness values are DAY
// levels, and everything dims to nightBrightness between sunset and
// sunrise (from Open-Meteo for the configured lat/lon).
bool dayNightEnabled = false;
uint8_t nightBrightness = 1;
int sunriseMinutes = -1;   // local minutes-of-day; -1 = unknown
int sunsetMinutes = -1;
bool sunTimesValid = false;

bool aircraftPresent = false;
char aircraftCarrier[80] = "";
char aircraftType[80] = "";
char aircraftOrigin[80] = "";
char aircraftDest[80] = "";
String aircraftCallsign = "";
String aircraftLockedHex = "";
unsigned long lastAircraftPollMs = 0;
const unsigned long AIRCRAFT_POLL_INTERVAL_MS = 2000;
// Backoff: hammering an unreachable API host every 2s exhausted the LwIP
// socket pool and wedged the whole WiFi stack (the real reboot cause seen
// in v30 logs — every fetch then failed with -1 until the RTC watchdog
// rebooted). After repeated failures we poll far less often so sockets
// drain; snap back to 2s the instant a poll succeeds.
unsigned long aircraftPollIntervalMs = 2000;
uint8_t aircraftConsecutiveFails = 0;

// Detail-session state machine: while aircraftPresent + session hasn't
// timed out, we rotate through 4 fields (Carrier, Type, From, To) each
// shown for FieldSec seconds with a BlankMs pause between them.
unsigned long aircraftSessionStartedMs = 0;
uint8_t aircraftFieldIdx = 0;   // 0=Carrier+Type phase, 1=Departure+Arrival phase
bool aircraftFieldBlank = false;
unsigned long aircraftFieldStartedMs = 0;
bool aircraftReturnedToClock = false;   // true after session timed out; cleared when a new aircraft arrives

// Scrolling text state
int scrollOffsetPx = 0;
unsigned long scrollStartMs = 0;   // global — reset from multiple places (news flip, aircraft field change)

bool isAPMode = false;
// Master "Only Clock" mode: when on, the whole 16x64 shows a big HH:MM
// clock and nothing else (no news, aircraft, weather icon, date).
bool masterClockMode = false;
// Which big-clock font to use in Only Clock mode: 0=Original (first
// hand-drawn attempt), 1=Classic (real proven font, default), 2=7-Segment,
// 3=Soft Rounded.
uint8_t bigFontStyle = 1;
// Test pattern: lights all pixels for 3s when triggered from the System card.
bool testPatternActive = false;
unsigned long testPatternUntilMs = 0;
bool timeSynced = false;
bool colonOn = true;
struct tm cachedTime;
bool cachedTimeValid = false;
unsigned long lastTimeCacheMs = 0;

WeatherIcon currentWeather = W_NONE;
unsigned long lastWeatherFetchMs = 0;
const unsigned long WEATHER_FETCH_INTERVAL_MS = 15UL * 60 * 1000;

const char* DOW_NAMES[7]    = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
const char* MONTH_NAMES[12] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};

char clockText[16] = "";
char dateText[24] = "";
char statusLine[64] = "booting";

// ---------- LOG BUFFER ----------
#define LOG_BUFFER_LINES 40
String logBuffer[LOG_BUFFER_LINES];
uint8_t logBufferHead = 0;

void logMsg(const String& msg) {
  Serial.println(msg);
  char prefix[16];
  snprintf(prefix, sizeof(prefix), "[%lus] ", millis() / 1000);
  logBuffer[logBufferHead] = String(prefix) + msg;
  logBufferHead = (logBufferHead + 1) % LOG_BUFFER_LINES;
}

String getLogsAsText() {
  String out = "";
  for (uint8_t i = 0; i < LOG_BUFFER_LINES; i++) {
    uint8_t idx = (logBufferHead + i) % LOG_BUFFER_LINES;
    if (logBuffer[idx].length() > 0) out += logBuffer[idx] + "\n";
  }
  return out;
}

WebServer server(80);

// ============================================================================
// CONFIG
// ============================================================================
void loadConfig() {
  prefs.begin("clock", false);
  wifiSsid     = prefs.getString("ssid", DEFAULT_WIFI_SSID);
  wifiPassword = prefs.getString("pass", DEFAULT_WIFI_PASSWORD);
  use24h       = prefs.getBool("use24h", DEFAULT_USE_24H);
  brightness   = prefs.getUChar("bright", DEFAULT_BRIGHTNESS);
  cityIndex    = prefs.getUChar("city", DEFAULT_CITY_INDEX);
  clockOnTop   = prefs.getBool("clkTop", true);
  masterClockMode = prefs.getBool("masterClk", false);
  bigFontStyle = prefs.getUChar("bigFont", 1);
  if (bigFontStyle > 4) bigFontStyle = 1;
  weatherLat   = prefs.getFloat("wLat", DEFAULT_WEATHER_LAT);
  weatherLon   = prefs.getFloat("wLon", DEFAULT_WEATHER_LON);
  newsEnabled  = prefs.getBool("newsOn", false);
  newsSourceMask = prefs.getUShort("newsMask", 0x0001);
  newsCycleS   = prefs.getUShort("newsCycS", 30);
  newsScrollSpeed = prefs.getUChar("newsSpd", 5);
  newsOrderMode   = prefs.getUChar("newsOrd", 0);
  newsLetterSpacing = prefs.getUChar("newsSpc", 2);
  if (newsLetterSpacing < 1) newsLetterSpacing = 1;
  if (newsLetterSpacing > 5) newsLetterSpacing = 5;
  if (newsScrollSpeed < 1) newsScrollSpeed = 1;
  if (newsScrollSpeed > 10) newsScrollSpeed = 10;
  aircraftEnabled = prefs.getBool("acOn", false);
  aircraftApiHost = prefs.getString("acHost", "192.168.1.100");
  aircraftApiPort = prefs.getUShort("acPort", 8000);
  aircraftSessionSec = prefs.getUShort("acSess", 45);
  aircraftFieldSec   = prefs.getUShort("acFld", 7);
  aircraftFieldSec2  = prefs.getUShort("acFld2", 7);
  aircraftBlankMs    = prefs.getUShort("acBlank", 1000);
  aircraftScrollMs   = prefs.getUChar("acScr", 40);
  aircraftMaxChars   = prefs.getUChar("acMax", 10);
  brightnessClock    = prefs.getUChar("brClk", brightness);
  brightnessNews     = prefs.getUChar("brNws", brightness);
  brightnessAircraft = prefs.getUChar("brAir", brightness);
  dayNightEnabled = prefs.getBool("dnOn", false);
  nightBrightness = prefs.getUChar("brNight", 1);
  if (nightBrightness > 15) nightBrightness = 15;
  prefs.end();
  if (cityIndex >= CITY_COUNT) cityIndex = DEFAULT_CITY_INDEX;
  if (newsCycleS < 8) newsCycleS = 8;
  if (newsCycleS > 120) newsCycleS = 120;
  if (aircraftFieldSec < 1) aircraftFieldSec = 1;
  if (aircraftFieldSec > 60) aircraftFieldSec = 60;
  if (aircraftFieldSec2 < 1) aircraftFieldSec2 = 1;
  if (aircraftFieldSec2 > 60) aircraftFieldSec2 = 60;
  if (aircraftBlankMs > 5000) aircraftBlankMs = 5000;
  if (aircraftScrollMs < 5) aircraftScrollMs = 5;
  if (aircraftScrollMs > 200) aircraftScrollMs = 200;
  if (aircraftMaxChars < 4) aircraftMaxChars = 4;
  if (aircraftMaxChars > 30) aircraftMaxChars = 30;
  if (brightnessClock > 15) brightnessClock = 15;
  if (brightnessNews > 15) brightnessNews = 15;
  if (brightnessAircraft > 15) brightnessAircraft = 15;
}

void saveConfig(String ssid, String pass, bool h24, uint8_t bright, uint8_t city,
                bool clkTop, float lat, float lon, bool newsOn, uint16_t newsMask, uint16_t newsCyc,
                bool acOn, String acHost, uint16_t acPort,
                uint8_t newsSpd, uint16_t acSess, uint16_t acFld, uint16_t acFld2, uint16_t acBlank,
                uint8_t acScr, uint8_t acMax,
                uint8_t brClk, uint8_t brNws, uint8_t brAir) {
  prefs.begin("clock", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.putBool("use24h", h24);
  prefs.putUChar("bright", bright);
  prefs.putUChar("city", city);
  prefs.putBool("clkTop", clkTop);
  prefs.putFloat("wLat", lat);
  prefs.putFloat("wLon", lon);
  prefs.putBool("newsOn", newsOn);
  prefs.putUShort("newsMask", newsMask);
  prefs.putUShort("newsCycS", newsCyc);
  prefs.putBool("acOn", acOn);
  prefs.putString("acHost", acHost);
  prefs.putUShort("acPort", acPort);
  prefs.putUChar("newsSpd", newsSpd);
  prefs.putUShort("acSess", acSess);
  prefs.putUShort("acFld", acFld);
  prefs.putUShort("acFld2", acFld2);
  prefs.putUShort("acBlank", acBlank);
  prefs.putUChar("acScr", acScr);
  prefs.putUChar("acMax", acMax);
  prefs.putUChar("brClk", brClk);
  prefs.putUChar("brNws", brNws);
  prefs.putUChar("brAir", brAir);
  prefs.end();
  wifiSsid = ssid; wifiPassword = pass;
  use24h = h24; brightness = bright; cityIndex = city;
  clockOnTop = clkTop; weatherLat = lat; weatherLon = lon;
  newsEnabled = newsOn; newsSourceMask = newsMask; newsCycleS = newsCyc;
  aircraftEnabled = acOn; aircraftApiHost = acHost; aircraftApiPort = acPort;
  newsScrollSpeed = newsSpd;
  aircraftSessionSec = acSess; aircraftFieldSec = acFld; aircraftFieldSec2 = acFld2; aircraftBlankMs = acBlank;
  aircraftScrollMs = acScr; aircraftMaxChars = acMax;
  brightnessClock = brClk; brightnessNews = brNws; brightnessAircraft = brAir;
}

// ============================================================================
// WIFI + NTP
// ============================================================================
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(300); Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    isAPMode = false;
    logMsg("WiFi connected: " + WiFi.localIP().toString());
  } else {
    logMsg("WiFi timeout — starting AP mode");
    WiFi.mode(WIFI_AP);
    WiFi.softAP("Wifi Clock");
    isAPMode = true;
    logMsg("AP IP: " + WiFi.softAPIP().toString());
  }
}

void syncTime() {
  if (isAPMode || WiFi.status() != WL_CONNECTED) return;
  configTime(0, 0, NTP_SERVER_1, NTP_SERVER_2);
  setenv("TZ", CITY_LIST[cityIndex].posixTZ, 1);
  tzset();
  struct tm ti;
  if (getLocalTime(&ti, 5000)) { timeSynced = true; logMsg("Time synced"); }
  else logMsg("Time sync failed");
}

void refreshTimeCache() {
  if (!timeSynced) return;
  if (millis() - lastTimeCacheMs < 1000 && cachedTimeValid) return;
  lastTimeCacheMs = millis();
  time_t now; time(&now);
  localtime_r(&now, &cachedTime);
  cachedTimeValid = true;
}

// ============================================================================
// WEATHER FETCH
// ============================================================================
int extractIntAfterKey(const String& json, const String& key, int searchFrom) {
  int idx = json.indexOf("\"" + key + "\"", searchFrom);
  if (idx < 0) return -1;
  idx = json.indexOf(':', idx);
  if (idx < 0) return -1;
  idx++;
  while (idx < (int)json.length() && json[idx] == ' ') idx++;
  int start = idx;
  while (idx < (int)json.length() && (isDigit(json[idx]) || json[idx] == '-')) idx++;
  if (idx == start) return -1;
  return json.substring(start, idx).toInt();
}

void fetchWeather() {
  if (isAPMode || WiFi.status() != WL_CONNECTED) return;
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(5);   // seconds — for TLS handshake
  HTTPClient http;
  http.setConnectTimeout(3000);
  http.setTimeout(5000);   // ms — for the actual HTTP transaction
  char url[240];
  snprintf(url, sizeof(url),
    "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f&current_weather=true&daily=sunrise,sunset&timezone=auto",
    weatherLat, weatherLon);
  http.begin(client, url);
  int code = http.GET();
  if (code == 200) {
    String p = http.getString();
    int blk = p.indexOf("\"current_weather\":");
    if (blk < 0) blk = 0;
    int w = extractIntAfterKey(p, "weathercode", blk);
    if (w >= 0) {
      currentWeather = mapWeatherCode(w);
      logMsg("Weather code " + String(w) + " -> icon " + String((int)currentWeather));
    } else logMsg("Weather OK but no weathercode found");

    // Sunrise/sunset. The response has a "daily_units" block that also
    // contains the words sunrise/sunset (as unit strings) BEFORE the real
    // "daily" data. Anchor to "daily" first, then find sunset strictly
    // after sunrise so the two never resolve to the same 'T' (the v29 bug
    // that produced identical rise=set times).
    int dailyIdx = p.indexOf("\"daily\":");
    int base = (dailyIdx >= 0) ? dailyIdx : 0;
    int srIdx = p.indexOf("\"sunrise\"", base);
    int ssIdx = (srIdx >= 0) ? p.indexOf("\"sunset\"", srIdx) : p.indexOf("\"sunset\"", base);
    auto parseT = [&](int fromIdx) -> int {
      if (fromIdx < 0) return -1;
      int t = p.indexOf('T', fromIdx);
      if (t < 0 || t + 5 >= (int)p.length()) return -1;
      int hh = p.substring(t + 1, t + 3).toInt();
      int mm = p.substring(t + 4, t + 6).toInt();
      return hh * 60 + mm;
    };
    int sr = parseT(srIdx), ss = parseT(ssIdx);
    if (sr >= 0 && ss >= 0 && sr != ss) {
      sunriseMinutes = sr; sunsetMinutes = ss; sunTimesValid = true;
      char sb[48];
      snprintf(sb, sizeof(sb), "Sun: rise=%02d:%02d set=%02d:%02d", sr/60, sr%60, ss/60, ss%60);
      logMsg(sb);
    }
  } else logMsg("Weather fetch failed HTTP " + String(code));
  http.end();
}

// ============================================================================
// NEWS FETCH — round-robin through enabled sources
// ============================================================================
String decodeBasicEntities(String s) {
  s.replace("&amp;", "&"); s.replace("&quot;", "\""); s.replace("&apos;", "'");
  s.replace("&#039;", "'"); s.replace("&lt;", "<"); s.replace("&gt;", ">");
  return s;
}

int nextEnabledSource(uint8_t from) {
  for (uint8_t i = 0; i < NEWS_SOURCE_COUNT; i++) {
    uint8_t idx = (from + i) % NEWS_SOURCE_COUNT;
    if (newsSourceMask & (1 << idx)) return idx;
  }
  return -1;
}

// Shuffle-bag: random enabled source not yet shown this cycle; resets
// when all enabled sources have been shown (each shown once before repeats).
int nextRandomSource() {
  uint8_t candidates[16]; uint8_t count = 0;
  for (uint8_t i = 0; i < NEWS_SOURCE_COUNT; i++)
    if ((newsSourceMask & (1 << i)) && !(newsShownMask & (1 << i))) candidates[count++] = i;
  if (count == 0) {
    newsShownMask = 0;
    for (uint8_t i = 0; i < NEWS_SOURCE_COUNT; i++)
      if (newsSourceMask & (1 << i)) candidates[count++] = i;
    if (count == 0) return -1;
  }
  uint8_t pick = candidates[esp_random() % count];
  newsShownMask |= (1 << pick);
  return pick;
}

int nextNewsSource() {
  if (newsOrderMode == 1) return nextRandomSource();
  int idx = nextEnabledSource(newsRotationIdx);
  if (idx >= 0) newsRotationIdx = (idx + 1) % NEWS_SOURCE_COUNT;
  return idx;
}

bool fetchNewsFromSource(uint8_t idx) {
  if (isAPMode || WiFi.status() != WL_CONNECTED) return false;
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(5);
  HTTPClient http;
  http.setConnectTimeout(3000);
  http.setTimeout(5000);
  // Some sources redirect (e.g. Moneycontrol http->https, 302). Follow
  // them so we don't fail with "HTTP 302" — the old code required a
  // direct 200.
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.begin(client, NEWS_SOURCES[idx].url);
  bool ok = false;
  int code = http.GET();
  if (code == 200) {
    String p = http.getString();
    int itemIdx = p.indexOf("<item>");
    if (itemIdx < 0) itemIdx = p.indexOf("<item ");
    int searchFrom = (itemIdx >= 0) ? itemIdx : 0;
    int ts = p.indexOf("<title>", searchFrom);
    if (ts >= 0) {
      ts += 7;
      int te = p.indexOf("</title>", ts);
      if (te > ts) {
        String h = p.substring(ts, te);
        h.replace("<![CDATA[", ""); h.replace("]]>", "");
        h = decodeBasicEntities(h);
        h.trim();

        // Remove any self-referential feed-title prefix the source
        // includes in each item title (BBC does this in some feeds).
        String feedName = String(NEWS_SOURCES[idx].name);
        String prefix1 = feedName + ": ";
        String prefix2 = feedName + " - ";
        if (h.startsWith(prefix1)) h = h.substring(prefix1.length());
        else if (h.startsWith(prefix2)) h = h.substring(prefix2.length());

        // Moderate sanitization (deliberately not aggressive per your ask):
        //   - collapse " and " -> " & " (saves 3 chars, still readable)
        //   - strip surrounding smart quotes and stray ASCII double quotes
        //   - collapse multiple spaces to one
        // Kept intentionally light so the meaning of headlines isn't
        // mangled. Question marks are LEFT IN — they carry actual
        // meaning in headlines ("Will X happen?"), and stripping them
        // makes news read like a statement of fact instead of a question.
        h.replace(" and ", " & ");
        h.replace("\"", "");
        h.replace("\u201C", "");   // opening curly double quote
        h.replace("\u201D", "");   // closing curly double quote
        while (h.indexOf("  ") >= 0) h.replace("  ", " ");
        h.trim();

        String tagged = String(NEWS_SOURCES[idx].tag) + ": " + h;
        tagged.toCharArray(newsHeadline, sizeof(newsHeadline));
        newsAvailable = true;
        ok = true;
        logMsg(tagged);
      }
    } else logMsg("News: no <title> after <item>");
  } else logMsg("News fetch FAILED: " + String(NEWS_SOURCES[idx].name) + " HTTP " + String(code));
  http.end();
  logMsg("Heap after news fetch: " + String(ESP.getFreeHeap()));
  return ok;
}

// ============================================================================
// AIRCRAFT FETCH — poll aviation-data-api for overhead aircraft
// ============================================================================
void fetchAircraftRoute(const String& callsign) {
  if (callsign.length() == 0) return;
  WiFiClient client;
  client.setTimeout(3);
  HTTPClient http;
  http.setConnectTimeout(2000);
  http.setTimeout(4000);
  String url = "http://" + aircraftApiHost + ":" + String(aircraftApiPort) + "/routes/" + callsign;
  http.begin(client, url);
  int code = http.GET();
  if (code != 200) {
    logMsg("Route fetch HTTP " + String(code) + " for " + callsign);
    http.end();
    return;
  }
  String p = http.getString();
  http.end();

  // Look for "found":true first
  int fIdx = p.indexOf("\"found\"");
  bool found = false;
  if (fIdx >= 0) {
    int c = p.indexOf(':', fIdx);
    if (c >= 0 && p.indexOf("true", c) == c + 1 + (p[c + 1] == ' ' ? 1 : 0)) found = true;
  }
  if (!found) {
    aircraftOrigin[0] = 0;
    aircraftDest[0] = 0;
    logMsg("Route not found for " + callsign);
    return;
  }

  // Extract origin.short_name and destination.short_name via lightweight
  // string search. Format: "origin":{"short_name":"Amsterdam", ...}
  auto extractNested = [&](const char* parent, const char* key) -> String {
    int p1 = p.indexOf(String("\"") + parent + "\"");
    if (p1 < 0) return "";
    int brace = p.indexOf('{', p1);
    if (brace < 0) return "";
    int closeBrace = p.indexOf('}', brace);
    if (closeBrace < 0) return "";
    int k = p.indexOf(String("\"") + key + "\"", brace);
    if (k < 0 || k > closeBrace) return "";
    int colon = p.indexOf(':', k);
    if (colon < 0) return "";
    int q1 = p.indexOf('"', colon);
    if (q1 < 0) return "";
    int q2 = p.indexOf('"', q1 + 1);
    if (q2 < 0) return "";
    return p.substring(q1 + 1, q2);
  };

  String o = extractNested("origin", "short_name");
  if (o.length() == 0) o = extractNested("origin", "name");
  String d = extractNested("destination", "short_name");
  if (d.length() == 0) d = extractNested("destination", "name");

  o.toCharArray(aircraftOrigin, sizeof(aircraftOrigin));
  d.toCharArray(aircraftDest, sizeof(aircraftDest));
  logMsg("Route: " + o + " -> " + d);
}

// ============================================================================
// AIRLINE NAME SHORTENING
// ============================================================================
// The aviation-data-api returns full airline names ("KLM Royal Dutch
// Airlines", "Scandinavian Airlines System") which are much longer than
// aircraftMaxChars, forcing them to scroll constantly. The old ESP8266
// sketches didn't shorten these (they just displayed doc["carrier"]
// as-is) — this table is new, covering common European/global carriers
// so the common case fits statically. Anything not in the table falls
// through unchanged (still scrolls if long, but at least doesn't break).
struct AirlineAbbrev { const char* full; const char* shortName; };
const AirlineAbbrev AIRLINE_ABBREVS[] = {
  { "KLM Royal Dutch Airlines", "KLM" },
  { "Scandinavian Airlines System", "SAS" },
  { "Scandinavian Airlines", "SAS" },
  { "British Airways", "BA" },
  { "Deutsche Lufthansa", "Lufthansa" },
  { "Air France", "Air France" },
  { "easyJet Europe", "easyJet" },
  { "easyJet Airline", "easyJet" },
  { "Ryanair Holdings", "Ryanair" },
  { "Ryanair DAC", "Ryanair" },
  { "Transavia Airlines", "Transavia" },
  { "Turkish Airlines", "Turkish" },
  { "Emirates Airlines", "Emirates" },
  { "Qatar Airways", "Qatar" },
  { "United Airlines", "United" },
  { "Delta Air Lines", "Delta" },
  { "American Airlines", "American" },
  { "Singapore Airlines", "Singapore" },
  { "Cathay Pacific Airways", "Cathay Pacific" },
  { "Finnair Oyj", "Finnair" },
  { "Icelandair Group", "Icelandair" },
  { "Austrian Airlines", "Austrian" },
  { "Swiss International Air Lines", "Swiss" },
  { "Brussels Airlines", "Brussels" },
  { "Norwegian Air Shuttle", "Norwegian" },
  { "Vueling Airlines", "Vueling" },
  { "Iberia Lineas Aereas", "Iberia" },
  { "TAP Air Portugal", "TAP" },
  { "Wizz Air Hungary", "Wizz Air" },
  { "NetJets Europe", "NetJets" },
  { "Commandement Du Transport Aerien Militaire Francais", "French AF" },
  { "Royal Netherlands Air Force", "RNLAF" },
  { "Royal Air Force", "RAF" },
  { "United States Air Force", "USAF" },
};
const uint8_t AIRLINE_ABBREV_COUNT = sizeof(AIRLINE_ABBREVS) / sizeof(AIRLINE_ABBREVS[0]);

String shortenAirlineName(const String& full) {
  for (uint8_t i = 0; i < AIRLINE_ABBREV_COUNT; i++) {
    if (full.equalsIgnoreCase(AIRLINE_ABBREVS[i].full)) {
      return String(AIRLINE_ABBREVS[i].shortName);
    }
  }
  return full;   // no match — leave as-is (will scroll if it's long)
}

void fetchAircraft() {
  if (!aircraftEnabled || isAPMode || WiFi.status() != WL_CONNECTED) return;
  WiFiClient client;
  client.setTimeout(3);
  HTTPClient http;
  http.setConnectTimeout(2000);
  http.setTimeout(4000);
  String url = "http://" + aircraftApiHost + ":" + String(aircraftApiPort) + "/led-matrix-state";
  http.begin(client, url);
  int code = http.GET();
  if (code != 200) {
    static unsigned long lastFailLog = 0;
    if (millis() - lastFailLog > 60000) {
      lastFailLog = millis();
      logMsg("Aircraft API unreachable, HTTP " + String(code));
    }
    aircraftPresent = false;
    http.end();
    if (aircraftConsecutiveFails < 250) aircraftConsecutiveFails++;
    if (aircraftConsecutiveFails >= 3) {
      aircraftPollIntervalMs = 30000;
      if (aircraftConsecutiveFails == 3) logMsg("Aircraft API failing — backing off to 30s polls");
    }
    return;
  }
  if (aircraftConsecutiveFails > 0) {
    aircraftConsecutiveFails = 0;
    aircraftPollIntervalMs = AIRCRAFT_POLL_INTERVAL_MS;
    logMsg("Aircraft API back — resuming 2s polls");
  }
  String p = http.getString();
  http.end();

  int aIdx = p.indexOf("\"aircraft_present\"");
  bool present = false;
  if (aIdx >= 0) {
    int colon = p.indexOf(':', aIdx);
    if (colon >= 0 && p.indexOf("true", colon) == colon + 1 + (p[colon + 1] == ' ' ? 1 : 0)) {
      present = true;
    }
  }
  if (!present) {
    if (aircraftPresent) logMsg("Aircraft no longer overhead");
    aircraftPresent = false;
    aircraftLockedHex = "";
    aircraftOrigin[0] = 0;
    aircraftDest[0] = 0;
    return;
  }

  auto extractString = [&](const char* key) -> String {
    int k = p.indexOf(String("\"") + key + "\"");
    if (k < 0) return "";
    int c = p.indexOf(':', k);
    if (c < 0) return "";
    int q1 = p.indexOf('"', c);
    if (q1 < 0) return "";
    int q2 = p.indexOf('"', q1 + 1);
    if (q2 < 0) return "";
    return p.substring(q1 + 1, q2);
  };

  String hex = extractString("hex");
  String callsign = extractString("callsign");
  String carrier = extractString("carrier");
  // type_name is the full display name (e.g. "Airbus A320neo") resolved
  // by the API. Fall through to the raw ICAO type code, then the "type"
  // field (older API versions), for compatibility.
  String type = extractString("type_name");
  if (type.length() == 0) type = extractString("type");
  if (type.length() == 0) type = extractString("aircraft_type");
  if (carrier.length() == 0) carrier = "Aircraft";
  carrier = shortenAirlineName(carrier);
  // Leave type blank if truly unknown — render() shows "-" for a blank field
  // rather than the literal text "Unknown Type" scrolling forever.

  bool newAircraft = (hex != aircraftLockedHex);
  aircraftLockedHex = hex;
  aircraftCallsign = callsign;

  // Only rewrite the display buffers when the content actually changes.
  // Rewriting them every 2s poll (even with identical text) can glitch a
  // scroll that's mid-pass. On a new aircraft we always refresh.
  char newCarrier[80]; char newType[80];
  carrier.toCharArray(newCarrier, sizeof(newCarrier));
  type.toCharArray(newType, sizeof(newType));
  if (newAircraft || strcmp(newCarrier, aircraftCarrier) != 0) {
    strncpy(aircraftCarrier, newCarrier, sizeof(aircraftCarrier));
  }
  if (newAircraft || strcmp(newType, aircraftType) != 0) {
    strncpy(aircraftType, newType, sizeof(aircraftType));
  }

  if (!aircraftPresent || newAircraft) {
    logMsg("Aircraft overhead: " + carrier + " (" + (type.length() ? type : String("type unknown")) + ")");
    // Reset the field state machine: new session, start at Carrier field
    aircraftSessionStartedMs = millis();
    aircraftFieldIdx = 0;
    aircraftFieldBlank = false;
    aircraftFieldStartedMs = millis();
    aircraftReturnedToClock = false;
    scrollOffsetPx = 0;
    scrollStartMs = millis();
    if (callsign.length() > 0) fetchAircraftRoute(callsign);
  }
  aircraftPresent = true;
}

// ============================================================================
// RENDER — build clockText/dateText, then paint into frameBuffer
// ============================================================================
void buildClockText() {
  if (!timeSynced || !cachedTimeValid) {
    strcpy(clockText, "Syncing");
    return;
  }
  int hour = cachedTime.tm_hour;
  const char* ampm = "";
  if (!use24h) {
    ampm = (hour >= 12) ? "PM" : "AM";
    hour = hour % 12; if (hour == 0) hour = 12;
  }
  const char* sep = colonOn ? ":" : " ";
  if (use24h) snprintf(clockText, sizeof(clockText), "%02d%s%02d", hour, sep, cachedTime.tm_min);
  else        snprintf(clockText, sizeof(clockText), "%02d%s%02d %s", hour, sep, cachedTime.tm_min, ampm);
}

void buildDateText() {
  if (!timeSynced || !cachedTimeValid) { dateText[0] = 0; return; }
  snprintf(dateText, sizeof(dateText), "%s %d %s",
           DOW_NAMES[cachedTime.tm_wday], cachedTime.tm_mday, MONTH_NAMES[cachedTime.tm_mon]);
}

// Applies brightness per PHYSICAL ROW. MAX7219 stores intensity per
// device (per 8x8 chip), so brightness for the clock row and the date/
// news/aircraft row is genuinely independent. Uses the confirmed
// device layout:
//   Top row = devices 8..15 (device 15 leftmost)
//   Bottom row = devices 0..7 (device 7 leftmost)
// Which physical row hosts which content depends on clockOnTop.
// Tracks last-applied per row so we don't spam SPI writes.
uint8_t lastAppliedTopBrightness = 255;
uint8_t lastAppliedBotBrightness = 255;
void applyRowBrightness(uint8_t topLevel, uint8_t botLevel) {
  if (topLevel != lastAppliedTopBrightness) {
    for (uint8_t d = 8; d <= 15; d++) mx.control(d, MD_MAX72XX::INTENSITY, topLevel);
    lastAppliedTopBrightness = topLevel;
  }
  if (botLevel != lastAppliedBotBrightness) {
    for (uint8_t d = 0; d <= 7; d++) mx.control(d, MD_MAX72XX::INTENSITY, botLevel);
    lastAppliedBotBrightness = botLevel;
  }
}

// Pick clock/news/aircraft brightness for each physical row based on
// what's currently displayed there. Aircraft occupies both rows.
// True if local time is between sunset and sunrise. Falls back to false
// (day) if data is missing, so it never wrongly goes dark.
bool isNightTime() {
  if (!dayNightEnabled || !sunTimesValid || !timeSynced || !cachedTimeValid) return false;
  int nowMin = cachedTime.tm_hour * 60 + cachedTime.tm_min;
  if (sunsetMinutes > sunriseMinutes)
    return (nowMin >= sunsetMinutes) || (nowMin < sunriseMinutes);
  return false;
}

void applyBrightnessForCurrentState(bool aircraftActive, bool newsShowing) {
  if (isNightTime()) {
    applyRowBrightness(nightBrightness, nightBrightness);
    return;
  }
  uint8_t topLevel, botLevel;
  if (aircraftActive) {
    topLevel = brightnessAircraft;
    botLevel = brightnessAircraft;
  } else {
    uint8_t clockRowLevel = brightnessClock;
    uint8_t dateRowLevel  = newsShowing ? brightnessNews : brightnessClock;
    if (clockOnTop) { topLevel = clockRowLevel; botLevel = dateRowLevel; }
    else            { topLevel = dateRowLevel;  botLevel = clockRowLevel; }
  }
  applyRowBrightness(topLevel, botLevel);
}

// ============================================================================
// MASTER "ONLY CLOCK" MODE — 4 selectable font styles, each a fully
// self-contained render function (own glyph data, own layout, own
// fbClear/flush) so a bug in one style can never affect the others.
// Selected via bigFontStyle (0-3), persisted, chosen in the Clock card.
// ============================================================================

// ---- FONT 0: the very first hand-drawn attempt (chunky 10x16 block digits) ----
const uint16_t F0_DIGITS[10][16] = {
  {0x00FC,0x01FE,0x0387,0x0387,0x0387,0x0387,0x0387,0x0387,0x0387,0x0387,0x0387,0x0387,0x0387,0x0387,0x01FE,0x00FC},
  {0x0038,0x0078,0x00F8,0x0038,0x0038,0x0038,0x0038,0x0038,0x0038,0x0038,0x0038,0x0038,0x0038,0x0038,0x00FE,0x00FE},
  {0x00FC,0x01FE,0x0387,0x000F,0x000E,0x001C,0x0038,0x0070,0x00E0,0x01C0,0x0380,0x0380,0x0380,0x0380,0x03FF,0x03FF},
  {0x01FC,0x01FE,0x0007,0x0007,0x0007,0x001E,0x00FC,0x00FC,0x001E,0x0007,0x0007,0x0007,0x0007,0x01FE,0x01FC,0x00F8},
  {0x001C,0x003C,0x007C,0x00DC,0x019C,0x031C,0x031C,0x03FF,0x03FF,0x001C,0x001C,0x001C,0x001C,0x001C,0x001C,0x001C},
  {0x03FF,0x03FF,0x0380,0x0380,0x0380,0x03FC,0x03FE,0x000F,0x0007,0x0007,0x0007,0x0007,0x038E,0x03FE,0x01FC,0x00F8},
  {0x007C,0x00FC,0x01C0,0x0380,0x0380,0x0380,0x03FC,0x03FE,0x038F,0x0387,0x0387,0x0387,0x0387,0x01FE,0x01FC,0x00F8},
  {0x03FF,0x03FF,0x0007,0x000E,0x000E,0x001C,0x001C,0x0038,0x0038,0x0070,0x0070,0x00E0,0x00E0,0x01C0,0x01C0,0x01C0},
  {0x00FC,0x01FE,0x0387,0x0387,0x0387,0x01FE,0x00FC,0x00FC,0x01FE,0x0387,0x0387,0x0387,0x0387,0x0387,0x01FE,0x00FC},
  {0x00FC,0x01FE,0x0387,0x0387,0x0387,0x0387,0x0387,0x01FF,0x00F7,0x0007,0x0007,0x0007,0x0007,0x001C,0x01FE,0x01FC},
};
const uint16_t F0_COLON[16] = {0,0,0,0,0x0002,0x0002,0x0002,0,0,0x0002,0x0002,0x0002,0,0,0,0};
void f0_drawDigit(int x, uint8_t d) {
  for (uint8_t row = 0; row < 16; row++) {
    uint16_t bits = F0_DIGITS[d][row];
    for (uint8_t col = 0; col < 10; col++)
      if (bits & (1 << (9 - col))) fbSetPixel(x + col, row, true);
  }
}
void f0_drawColon(int x) {
  for (uint8_t row = 0; row < 16; row++) {
    uint16_t bits = F0_COLON[row];
    for (uint8_t col = 0; col < 3; col++)
      if (bits & (1 << (2 - col))) fbSetPixel(x + col, row, true);
  }
}
void renderBigClockFont0(int h1, int h2, int m1, int m2, bool suppressZero, bool use24, bool pm) {
  const int digitW = 10, colonW = 4, gap = 2;
  int totalW = 0;
  if (!suppressZero) totalW += digitW + gap;
  totalW += digitW + gap + colonW + gap + digitW + gap + digitW;
  int ampmW = use24 ? 0 : (1 + 11);
  int x = (64 - (totalW + ampmW)) / 2;
  if (x < 0) x = 0;
  if (!suppressZero) { f0_drawDigit(x, h1); x += digitW + gap; }
  f0_drawDigit(x, h2); x += digitW + gap;
  if (colonOn) f0_drawColon(x);
  x += colonW + gap;
  f0_drawDigit(x, m1); x += digitW + gap;
  f0_drawDigit(x, m2); x += digitW;
  if (!use24) { x += 1; drawString(x, 4, pm ? "PM" : "AM"); }
}

// ---- FONT 1: real proven font (Giovanni Bernardo / @cyb3rn0id), 2x scaled ----
struct BigGlyph { uint8_t width; const uint16_t* cols; };
const uint16_t BIGCOL_0[10] = {0x0FFC,0x0FFC,0x3FFF,0x3FFF,0x30C3,0x30C3,0x3033,0x3033,0x0FFC,0x0FFC};
const uint16_t BIGCOL_1[8]  = {0x0030,0x0030,0x000C,0x000C,0x3FFF,0x3FFF,0x3FFF,0x3FFF};
const uint16_t BIGCOL_2[10] = {0x3F03,0x3F03,0x3FC3,0x3FC3,0x30C3,0x30C3,0x30FF,0x30FF,0x303C,0x303C};
const uint16_t BIGCOL_3[10] = {0x3003,0x3003,0x30C3,0x30C3,0x30C3,0x30C3,0x3FFF,0x3FFF,0x0F3C,0x0F3C};
const uint16_t BIGCOL_4[10] = {0x00FF,0x00FF,0x00FF,0x00FF,0x00C0,0x00C0,0x3FC0,0x3FC0,0x3FFF,0x3FFF};
const uint16_t BIGCOL_5[10] = {0x30FF,0x30FF,0x30FF,0x30FF,0x30C3,0x30C3,0x3FC3,0x3FC3,0x0F03,0x0F03};
const uint16_t BIGCOL_6[10] = {0x0FFC,0x0FFC,0x3FFF,0x3FFF,0x30C3,0x30C3,0x30C3,0x30C3,0x0F00,0x0F00};
const uint16_t BIGCOL_7[10] = {0x000F,0x000F,0x000F,0x000F,0x3F03,0x3F03,0x3FC3,0x3FC3,0x003F,0x003F};
const uint16_t BIGCOL_8[10] = {0x0F3C,0x0F3C,0x3FFF,0x3FFF,0x30C3,0x30C3,0x30C3,0x30C3,0x0F3C,0x0F3C};
const uint16_t BIGCOL_9[10] = {0x003C,0x003C,0x30C3,0x30C3,0x30C3,0x30C3,0x3FFF,0x3FFF,0x0FFC,0x0FFC};
const uint16_t BIGCOL_COLON[4] = {0x3CF0,0x3CF0,0x3CF0,0x3CF0};
const BigGlyph BIG_GLYPHS[10] = {
  { 10, BIGCOL_0 }, { 8, BIGCOL_1 }, { 10, BIGCOL_2 }, { 10, BIGCOL_3 }, { 10, BIGCOL_4 },
  { 10, BIGCOL_5 }, { 10, BIGCOL_6 }, { 10, BIGCOL_7 }, { 10, BIGCOL_8 }, { 10, BIGCOL_9 },
};
uint8_t f1_drawDigit(int x, uint8_t digit) {
  const BigGlyph& g = BIG_GLYPHS[digit];
  for (uint8_t col = 0; col < g.width; col++) {
    uint16_t bits = g.cols[col];
    if (bits == 0) continue;
    for (uint8_t row = 0; row < 16; row++)
      if (bits & (1 << row)) fbSetPixel(x + col, row, true);
  }
  return g.width;
}
void f1_drawColon(int x) {
  for (uint8_t col = 0; col < 4; col++) {
    uint16_t bits = BIGCOL_COLON[col];
    for (uint8_t row = 0; row < 16; row++)
      if (bits & (1 << row)) fbSetPixel(x + col, row, true);
  }
}
void renderBigClockFont1(int h1, int h2, int m1, int m2, bool suppressZero, bool use24, bool pm) {
  const int gap = 2;
  int totalW = 0;
  if (!suppressZero) totalW += BIG_GLYPHS[h1].width + gap;
  totalW += BIG_GLYPHS[h2].width + gap + 4 + gap + BIG_GLYPHS[m1].width + gap + BIG_GLYPHS[m2].width;
  int ampmW = use24 ? 0 : (1 + 11);
  int x = (64 - (totalW + ampmW)) / 2;
  if (x < 0) x = 0;
  if (!suppressZero) { x += f1_drawDigit(x, h1) + gap; }
  x += f1_drawDigit(x, h2) + gap;
  if (colonOn) f1_drawColon(x);
  x += 4 + gap;
  x += f1_drawDigit(x, m1) + gap;
  x += f1_drawDigit(x, m2);
  if (!use24) { x += 1; drawString(x, 4, pm ? "PM" : "AM"); }
}

// ---- FONT 2: classic 7-segment (computed geometrically, not a bitmap table) ----
// Segments: a=top, b=upper-right, c=lower-right, d=bottom, e=lower-left,
// f=upper-left, g=middle. Cell is 8 wide x 16 tall, segment thickness 2px.
const uint8_t F2_SEGMENTS[10] = {
  0b0111111, // 0: a b c d e f
  0b0000110, // 1: b c
  0b1011011, // 2: a b g e d
  0b1001111, // 3: a b g c d
  0b1100110, // 4: f g b c
  0b1101101, // 5: a f g c d
  0b1111101, // 6: a f g e c d
  0b0000111, // 7: a b c
  0b1111111, // 8: all
  0b1101111, // 9: a b c d f g
};
// bit0=a,1=b,2=c,3=d,4=e,5=f,6=g
void f2_drawDigit(int x, uint8_t d) {
  uint8_t s = F2_SEGMENTS[d];
  const int W = 8, H = 16, TH = 2, mid = H / 2;
  auto hseg = [&](int y) { for (int xx = 1; xx < W - 1; xx++) for (int t = 0; t < TH; t++) fbSetPixel(x + xx, y + t, true); };
  auto vseg = [&](int cx, int y0, int y1) { for (int yy = y0; yy < y1; yy++) for (int t = 0; t < TH; t++) fbSetPixel(x + cx + t, yy, true); };
  if (s & 0x01) hseg(0);                    // a
  if (s & 0x40) hseg(mid - 1);               // g
  if (s & 0x08) hseg(H - TH);                // d
  if (s & 0x20) vseg(0, TH, mid);            // f
  if (s & 0x02) vseg(W - TH, TH, mid);       // b
  if (s & 0x10) vseg(0, mid, H - TH);        // e
  if (s & 0x04) vseg(W - TH, mid, H - TH);   // c
}
void f2_drawColon(int x) {
  for (int dy : {5, 6, 10, 11}) { fbSetPixel(x, dy, true); fbSetPixel(x + 1, dy, true); }
}
void renderBigClockFont2(int h1, int h2, int m1, int m2, bool suppressZero, bool use24, bool pm) {
  const int digitW = 8, colonW = 3, gap = 3;
  int totalW = 0;
  if (!suppressZero) totalW += digitW + gap;
  totalW += digitW + gap + colonW + gap + digitW + gap + digitW;
  int ampmW = use24 ? 0 : (2 + 11);
  int x = (64 - (totalW + ampmW)) / 2;
  if (x < 0) x = 0;
  if (!suppressZero) { f2_drawDigit(x, h1); x += digitW + gap; }
  f2_drawDigit(x, h2); x += digitW + gap;
  if (colonOn) f2_drawColon(x);
  x += colonW + gap;
  f2_drawDigit(x, m1); x += digitW + gap;
  f2_drawDigit(x, m2); x += digitW;
  if (!use24) { x += 2; drawString(x, 4, pm ? "PM" : "AM"); }
}

// ---- FONT 3: soft rounded proportional digits ----
const uint8_t FONT3_SOFT[10][13] PROGMEM = {
  {60,252,206,206,206,206,206,206,206,206,206,126,60},
  {24,56,120,24,24,24,24,24,24,24,24,126,126},
  {60,126,195,3,6,12,24,48,96,192,192,255,255},
  {60,126,195,3,7,30,30,7,3,195,126,60,0},
  {14,30,54,102,198,198,255,255,3,3,3,3,0},
  {255,192,192,192,254,62,3,3,3,195,126,60,0},
  {60,126,192,192,192,254,193,195,195,195,126,60,0},
  {255,255,3,6,6,12,12,24,24,48,48,48,0},
  {60,126,195,195,126,60,126,195,195,195,126,60,0},
  {60,126,195,195,195,127,63,3,3,195,126,60,0},
};
void f3_drawDigit(int x, uint8_t d) {
  for (uint8_t row = 0; row < 13; row++) {
    uint8_t bits = pgm_read_byte(&FONT3_SOFT[d][row]);
    if (bits == 0) continue;
    for (uint8_t col = 0; col < 8; col++)
      if (bits & (1 << (7 - col))) fbSetPixel(x + col, row + 1, true);
  }
}
void f3_drawColon(int x) {
  for (int dy : {4, 5, 8, 9}) fbSetPixel(x, dy + 1, true);
}
void renderBigClockFont3(int h1, int h2, int m1, int m2, bool suppressZero, bool use24, bool pm) {
  const int digitW = 8, colonW = 3, gap = 2;
  int totalW = 0;
  if (!suppressZero) totalW += digitW + gap;
  totalW += digitW + gap + colonW + gap + digitW + gap + digitW;
  int ampmW = use24 ? 0 : (1 + 11);
  int x = (64 - (totalW + ampmW)) / 2;
  if (x < 0) x = 0;
  if (!suppressZero) { f3_drawDigit(x, h1); x += digitW + gap; }
  f3_drawDigit(x, h2); x += digitW + gap;
  if (colonOn) f3_drawColon(x);
  x += colonW + gap;
  f3_drawDigit(x, m1); x += digitW + gap;
  f3_drawDigit(x, m2); x += digitW;
  if (!use24) { x += 1; drawString(x, 4, pm ? "PM" : "AM"); }
}

// ---- FONT 4: Bold (real, tested digit data — MrSmartus DMD font, verified) ----
// Source: gist.github.com/MrSmartus/31fc186fe0b14b38bbc0940883308f14, author-
// labeled "Tested." Column-major, split top/bottom 8-row halves (bit0=top row
// within each half). Digit '1' is narrower (4 wide) than the rest (7 wide).
const uint8_t F4_TOP[10][7] = {
  {0xFC,0xFE,0x03,0x03,0x03,0xFE,0xFC},
  {0x18,0x1C,0xFF,0xFF,0,0,0},
  {0x1E,0x1F,0x03,0x03,0x03,0xFF,0xFE},
  {0x0E,0x0F,0x03,0x83,0x83,0xFF,0x7E},
  {0x00,0xC0,0xF0,0x3C,0x0F,0xFF,0xFF},
  {0xFF,0xFF,0xC3,0xC3,0xC3,0xC3,0x83},
  {0xFE,0xFF,0xC3,0xC3,0xC3,0xC7,0x86},
  {0x07,0x07,0x03,0x03,0xC3,0xFF,0x7F},
  {0xBE,0xFF,0xC3,0xC3,0xC3,0xFF,0xBE},
  {0xFE,0xFF,0x03,0x03,0x03,0xFF,0xFE},
};
const uint8_t F4_BOT[10][7] = {
  {0x3F,0x7F,0xC0,0xC0,0xC0,0x7F,0x3F},
  {0x00,0x00,0xFF,0xFF,0,0,0},
  {0xE0,0xF0,0xF8,0xDC,0xCE,0xC7,0xC3},
  {0x70,0xF0,0xC0,0xC1,0xC1,0xFF,0x7F},
  {0x0F,0x0F,0x0C,0x0C,0x0C,0xFF,0xFF},
  {0x70,0xF0,0xC0,0xC0,0xC0,0xFF,0x7F},
  {0x7F,0xFF,0xC0,0xC0,0xC0,0xFF,0x7F},
  {0x00,0x00,0xF0,0xFE,0x1F,0x03,0x00},
  {0x7F,0xFF,0xC0,0xC0,0xC0,0xFF,0x7F},
  {0x61,0xE3,0xC3,0xC3,0xC3,0xFF,0x7F},
};
const uint8_t F4_WIDTH[10] = {7,4,7,7,7,7,7,7,7,7};
void f4_drawDigit(int x, uint8_t d) {
  uint8_t w = F4_WIDTH[d];
  for (uint8_t col = 0; col < w; col++) {
    uint8_t top = F4_TOP[d][col], bot = F4_BOT[d][col];
    for (uint8_t row = 0; row < 8; row++) {
      if (top & (1 << row)) fbSetPixel(x + col, row, true);
      if (bot & (1 << row)) fbSetPixel(x + col, row + 8, true);
    }
  }
}
void f4_drawColon(int x) {
  for (int dy : {6, 7, 9, 10}) { fbSetPixel(x, dy, true); fbSetPixel(x + 1, dy, true); }
}
void renderBigClockFont4(int h1, int h2, int m1, int m2, bool suppressZero, bool use24, bool pm) {
  const int gap = 2, colonW = 2;
  int totalW = 0;
  if (!suppressZero) totalW += F4_WIDTH[h1] + gap;
  totalW += F4_WIDTH[h2] + gap + colonW + gap + F4_WIDTH[m1] + gap + F4_WIDTH[m2];
  int ampmW = use24 ? 0 : (1 + 11);
  int x = (64 - (totalW + ampmW)) / 2;
  if (x < 0) x = 0;
  if (!suppressZero) { f4_drawDigit(x, h1); x += F4_WIDTH[h1] + gap; }
  f4_drawDigit(x, h2); x += F4_WIDTH[h2] + gap;
  if (colonOn) f4_drawColon(x);
  x += colonW + gap;
  f4_drawDigit(x, m1); x += F4_WIDTH[m1] + gap;
  f4_drawDigit(x, m2);
  x += F4_WIDTH[m2];
  if (!use24) { x += 1; drawString(x, 4, pm ? "PM" : "AM"); }
}

// ---- Dispatcher ----
void renderBigClock() {
  fbClear();
  uint8_t lvl = isNightTime() ? nightBrightness : brightnessClock;
  applyRowBrightness(lvl, lvl);
  if (!timeSynced || !cachedTimeValid) {
    drawStringCentered(0, 64, 5, "Syncing");
    fbFlush();
    return;
  }
  int hour = cachedTime.tm_hour;
  bool pm = hour >= 12;
  if (!use24h) { hour = hour % 12; if (hour == 0) hour = 12; }
  int mn = cachedTime.tm_min;
  int h1 = hour / 10, h2 = hour % 10, m1 = mn / 10, m2 = mn % 10;
  bool suppressLeadingZero = (!use24h && h1 == 0);

  switch (bigFontStyle) {
    case 0: renderBigClockFont0(h1, h2, m1, m2, suppressLeadingZero, use24h, pm); break;
    case 4: renderBigClockFont4(h1, h2, m1, m2, suppressLeadingZero, use24h, pm); break;
    default: renderBigClockFont1(h1, h2, m1, m2, suppressLeadingZero, use24h, pm); break;
  }
  fbFlush();
}


void render() {
  fbClear();

  // Test pattern: all pixels on for 3s (triggered from System card).
  if (testPatternActive) {
    if (millis() < testPatternUntilMs) {
      for (uint8_t r = 0; r < 16; r++) frameBuffer[r] = 0xFFFFFFFFFFFFFFFFULL;
      applyRowBrightness(brightnessClock, brightnessClock);
      fbFlush();
      return;
    } else {
      testPatternActive = false;
      logMsg("Test pattern ended");
    }
  }

  // Master "Only Clock" mode: big HH:MM fills the whole display, nothing else.
  if (masterClockMode && !isAPMode) {
    renderBigClock();
    return;
  }

  if (isAPMode) {
    applyRowBrightness(brightnessClock, brightnessClock);
    drawStringCentered(0, 64, 1, "Wifi Clock");
    char ipBuf[24];
    snprintf(ipBuf, sizeof(ipBuf), "%s", WiFi.softAPIP().toString().c_str());
    drawStringCentered(0, 64, 9, ipBuf);
    fbFlush();
    return;
  }

  int topY = 1;
  int botY = 9;

  bool aircraftActive = aircraftEnabled && aircraftPresent && !aircraftReturnedToClock;
  bool newsShowing    = !aircraftActive && newsEnabled && showingNews && newsAvailable;
  applyBrightnessForCurrentState(aircraftActive, newsShowing);

  // AIRCRAFT PRIORITY — highest. Two-phase display matching the old
  // ESP8266 sketches:
  //   Phase 0: TOP row = Carrier, BOTTOM row = Type
  //   Phase 1: TOP row = Departure (origin city), BOTTOM row = Arrival
  //            (destination city) — only shown if route info known
  // The Blank state briefly clears both rows for visual separation.
  // Each row's text is centered if short (<= aircraftMaxChars) or
  // scrolled independently if long.
  if (aircraftActive) {
    if (aircraftFieldBlank) {
      fbFlush();
      return;
    }
    const char* topTxt = "";
    const char* botTxt = "";
    if (aircraftFieldIdx == 0) {
      // Phase 0: Carrier + Type
      topTxt = aircraftCarrier;
      botTxt = aircraftType;
    } else {
      // Phase 1: Departure + Arrival
      topTxt = aircraftOrigin;
      botTxt = aircraftDest;
    }
    if (!topTxt || topTxt[0] == 0) topTxt = "-";
    if (!botTxt || botTxt[0] == 0) botTxt = "-";

    int topLen = strlen(topTxt);
    int botLen = strlen(botTxt);
    if (topLen <= aircraftMaxChars) drawStringCentered(0, 64, topY, topTxt);
    else                             drawScrollingString(0, 64, topY, topTxt, scrollOffsetPx);
    if (botLen <= aircraftMaxChars) drawStringCentered(0, 64, botY, botTxt);
    else                             drawScrollingString(0, 64, botY, botTxt, scrollOffsetPx);
    fbFlush();
    return;
  }

  // Normal clock+date mode
  buildClockText();
  buildDateText();
  int clockY = clockOnTop ? topY : botY;
  int dateY  = clockOnTop ? botY : topY;

  if (currentWeather != W_NONE) {
    drawWeatherIconAt(0, clockOnTop ? 0 : 8, currentWeather);
  }
  drawStringCentered(10, 54, clockY, clockText);

  if (newsShowing) {
    drawScrollingString(0, 64, dateY, newsHeadline, scrollOffsetPx, newsLetterSpacing - 2);
  } else {
    drawStringCentered(0, 64, dateY, dateText);
  }

  fbFlush();
}

// ============================================================================
// WEB UI — v32: 7 focused cards, UTF-8 charset fix
// ============================================================================
const char PAGE_HEAD[] PROGMEM = R"HTML(
<!DOCTYPE html><html><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>LED Matrix Clock</title><style>
:root{--accent:#4ade80;--bg:#0b0c0f;--card:#16181d;--border:#2a2d34;--text:#eee;--muted:#8b8f98;--danger:#ef4444}
*{box-sizing:border-box}body{font-family:-apple-system,sans-serif;max-width:480px;margin:0 auto;padding:16px 14px 40px;background:var(--bg);color:var(--text)}
header{margin-bottom:18px}
h1{font-size:18px;margin:0 0 2px;color:var(--accent)}
.meta{display:flex;flex-wrap:wrap;gap:8px 14px;font-size:12px;color:var(--muted);align-items:center}
.meta span{display:inline-flex;align-items:center;gap:4px}
.rssi-bar{display:inline-block;width:32px;height:10px;background:#222;border-radius:2px;overflow:hidden;vertical-align:middle}
.rssi-fill{height:100%;background:var(--accent);transition:width .3s}
.card{background:var(--card);border:1px solid var(--border);border-radius:10px;padding:14px;margin-bottom:12px}
.card h2{font-size:12px;text-transform:uppercase;letter-spacing:.06em;color:var(--muted);margin:0 0 12px;display:flex;align-items:center;gap:6px}
.card h2 .emoji{font-size:14px}
label{display:block;margin-top:10px;font-size:13px;color:var(--muted)}
label:first-child{margin-top:0}
input,select{width:100%;padding:8px 10px;margin-top:4px;background:#1e2025;border:1px solid var(--border);color:var(--text);border-radius:6px;font-size:14px}
input[type=range]{padding:0;accent-color:var(--accent)}
input[type=checkbox]{width:auto;margin:0 6px 0 0}
button{margin-top:12px;padding:9px 14px;width:100%;background:var(--accent);border:none;border-radius:6px;color:#08110b;font-weight:600;cursor:pointer;font-size:14px}
button.danger{background:var(--danger);color:#fff}
button.ghost{background:transparent;border:1px solid var(--border);color:var(--text)}
.slider-val{float:right;color:var(--accent);font-weight:600;font-size:13px}
.hint{font-size:11px;color:#666;margin-top:4px;line-height:1.4}
.row{display:flex;gap:8px;margin-top:8px}
.row>*{flex:1}
.src-list{max-height:260px;overflow-y:auto;padding:4px 0}
.src-item{display:flex;align-items:center;gap:8px;padding:5px 0;font-size:13px;border-bottom:1px solid #1f2228}
.src-item:last-child{border-bottom:none}
.src-item span{flex:1}
.stat-grid{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-top:8px}
.stat{background:#1a1c22;border:1px solid var(--border);border-radius:6px;padding:8px 10px}
.stat .k{font-size:10px;text-transform:uppercase;color:var(--muted);letter-spacing:.05em}
.stat .v{font-size:15px;font-weight:600;color:var(--text);margin-top:2px;font-variant-numeric:tabular-nums}
a.link{color:var(--accent);text-decoration:none;display:block;text-align:center;padding:10px;margin-top:8px;border:1px solid var(--border);border-radius:6px;font-size:13px}
a.link:hover{background:#1a1c22}
</style></head><body>
<header>
<h1>LED Matrix Clock</h1>
<div class="meta">
<span>%IP%</span>
<span>%FWVER%</span>
<span>Up <b id="uptime">%UPTIME%</b></span>
<span>WiFi <span class="rssi-bar"><span class="rssi-fill" id="rssi-fill" style="width:%RSSI_PCT%%"></span></span> <span id="rssi-db">%RSSI_DB%</span></span>
</div>
</header>
)HTML";

const char PAGE_FOOT[] PROGMEM = R"HTML(
<script>
async function refreshStatus(){
  try{
    const r=await fetch('/system/status');
    const s=await r.json();
    const d=Math.floor(s.uptime/86400),h=Math.floor((s.uptime%86400)/3600),m=Math.floor((s.uptime%3600)/60),sec=s.uptime%60;
    let up=d?d+'d '+h+'h':h?h+'h '+m+'m':m?m+'m '+sec+'s':sec+'s';
    document.getElementById('uptime').textContent=up;
    if(s.rssi<-100){document.getElementById('rssi-fill').style.width='5%';}
    else if(s.rssi>-50){document.getElementById('rssi-fill').style.width='100%';}
    else{document.getElementById('rssi-fill').style.width=Math.round((s.rssi+100)*2)+'%';}
    document.getElementById('rssi-db').textContent=s.rssi+' dBm';
  }catch(e){}
}
setInterval(refreshStatus,5000);
// Master "Only Clock" mode: gray out + disable the content/live-data cards
// so it's clear they have no effect while master mode is on.
function toggleMaster(on){
  // Cards to disable: Weather, Feeds, Aircraft (the display-content cards).
  // Network, Clock, Appearance, System stay usable.
  const forms=document.querySelectorAll('form');
  forms.forEach(f=>{
    const act=f.getAttribute('action')||'';
    if(act.includes('/weather')||act.includes('/feeds')||act.includes('/aircraft')){
      const card=f.closest('.card');
      if(card){card.style.opacity=on?'0.4':'1';card.style.pointerEvents=on?'none':'auto';}
    }
  });
}
document.addEventListener('DOMContentLoaded',()=>{
  const m=document.getElementById('masterclk');
  if(m) toggleMaster(m.checked);
});
</script>
</body></html>
)HTML";

const char CARD_NETWORK[] PROGMEM = R"HTML(
<div class="card"><h2><span class="emoji">🌐</span> Network</h2>
<form action="/save/network" method="POST">
<label>Wi-Fi SSID</label><input name="ssid" value="%WIFI_SSID%" required>
<label>Wi-Fi Password</label><input type="password" name="password" value="%WIFI_PASS%">
<div class="hint">Leave password blank to connect to an open network. Saved settings persist across reboots.</div>
<button type="submit">Save &amp; Reconnect</button>
</form></div>
)HTML";

const char CARD_CLOCK[] PROGMEM = R"HTML(
<div class="card"><h2><span class="emoji">🕐</span> Clock &amp; Time</h2>
<form action="/save/clock" method="POST">
<label style="display:flex;align-items:center;font-size:14px;color:var(--text);background:#1a1c22;padding:10px;border-radius:6px;margin-bottom:6px">
<input type="checkbox" name="masterclk" id="masterclk" value="1" %MASTERCHECKED% onchange="toggleMaster(this.checked)">
<b>Only Clock mode</b> &mdash; big time fills the whole display
</label>
<div class="hint" style="margin-bottom:10px">When on, a large HH:MM clock takes over the full 16×64 display and news, aircraft, weather &amp; date are hidden. Other cards are disabled while this is on.</div>
<label>Only Clock font style</label>
<select name="bigfont">
<option value="0" %BF0%>Original (first version)</option>
<option value="1" %BF1%>Classic (proven font)</option>
<option value="4" %BF4%>Bold (proven font, alternate style)</option>
</select>
<label>Time format</label>
<select name="use24h">
<option value="0" %OPT12%>12-hour (with AM/PM)</option>
<option value="1" %OPT24%>24-hour</option>
</select>
<label>City / timezone</label>
<select name="city">%CITY_OPTIONS%</select>
<label>Clock position</label>
<select name="clocktop">
<option value="1" %OPTTOP%>Clock on top</option>
<option value="0" %OPTBOT%>Clock on bottom</option>
</select>
<button type="submit">Save</button>
</form></div>
)HTML";

const char CARD_WEATHER[] PROGMEM = R"HTML(
<div class="card"><h2><span class="emoji">🌤️</span> Weather &amp; Environment</h2>
<form action="/save/weather" method="POST">
<label>Latitude (4 decimals ≈ 11m precision)</label>
<input name="wlat" type="number" step="0.0001" value="%WLAT%">
<label>Longitude (4 decimals ≈ 11m precision)</label>
<input name="wlon" type="number" step="0.0001" value="%WLON%">
<div class="hint">Used for weather icons and sunrise/sunset times (for day/night dimming). Updates every 15 minutes.</div>
<div style="margin-top:10px;padding:8px 10px;background:#1a1c22;border:1px solid var(--border);border-radius:6px;font-size:12px;color:var(--muted)">
%SUNINFO%
</div>
<button type="submit">Save</button>
</form></div>
)HTML";

const char CARD_FEEDS[] PROGMEM = R"HTML(
<div class="card"><h2><span class="emoji">📰</span> Content Feeds</h2>
<form action="/save/feeds" method="POST">
<label style="display:flex;align-items:center">
<input type="checkbox" name="newson" value="1" %NEWSCHECKED%>
Enable news ticker (alternates with date)
</label>
<label>Sources</label>
<div class="src-list">%NEWS_SOURCE_CHECKBOXES%</div>
<label>Alternate clock/date/news every <span class="slider-val" id="nv">%NEWSCYCLE%</span>s</label>
<input type="range" name="newscycle" min="10" max="120" value="%NEWSCYCLE%"
oninput="document.getElementById('nv').textContent=this.value">
<label>Scroll speed <span class="slider-val" id="nsv">%NEWSSPD%</span></label>
<input type="range" name="newsspd" min="1" max="10" value="%NEWSSPD%"
oninput="document.getElementById('nsv').textContent=this.value">
<div class="hint">1 = slow & readable, 10 = fast</div>
<label>Letter spacing <span class="slider-val" id="nspv">%NEWSSPACE%</span></label>
<input type="range" name="newsspace" min="1" max="5" value="%NEWSSPACE%"
oninput="document.getElementById('nspv').textContent=this.value">
<div class="hint">1 = tight, 2 = normal, 5 = wide</div>
<label>Feed order</label>
<div style="display:flex;gap:16px;margin-top:4px;font-size:14px">
<label style="display:flex;align-items:center;margin:0"><input type="radio" name="newsord" value="0" %NEWSORD0%>Sequential</label>
<label style="display:flex;align-items:center;margin:0"><input type="radio" name="newsord" value="1" %NEWSORD1%>Random (shuffle bag)</label>
</div>
<button type="submit">Save</button>
</form></div>
)HTML";

const char CARD_AIRCRAFT[] PROGMEM = R"HTML(
<div class="card"><h2><span class="emoji">✈️</span> Live Data — Aircraft Overhead</h2>
<form action="/save/aircraft" method="POST">
<label style="display:flex;align-items:center">
<input type="checkbox" name="acon" value="1" %ACCHECKED%>
Show aircraft when overhead (highest priority)
</label>
<label>API host</label><input name="achost" value="%ACHOST%">
<label>API port</label><input name="acport" type="number" min="1" max="65535" value="%ACPORT%">
<label>Detail session duration (s)</label>
<input type="number" name="acsess" min="0" max="600" value="%ACSESS%">
<div class="hint">0 = show while aircraft is present. Always completes one full cycle before returning to clock.</div>
<div class="row">
<div>
<label>Carrier &amp; Type (s)</label>
<input type="number" name="acfld" min="1" max="60" value="%ACFLD%">
</div>
<div>
<label>Departure &amp; Arrival (s)</label>
<input type="number" name="acfld2" min="1" max="60" value="%ACFLD2%">
</div>
</div>
<label>Blank gap between phases (ms)</label>
<input type="number" name="acblank" min="0" max="5000" value="%ACBLANK%">
<label>Scroll speed (ms per column, lower = faster)</label>
<input type="number" name="acscr" min="5" max="200" value="%ACSCR%">
<label>Max characters before scroll</label>
<input type="number" name="acmax" min="4" max="30" value="%ACMAX%">
<button type="submit">Save</button>
</form></div>
)HTML";

const char CARD_APPEARANCE[] PROGMEM = R"HTML(
<div class="card"><h2><span class="emoji">🎨</span> Appearance</h2>
<form action="/save/appearance" method="POST">
<label>Clock brightness <span class="slider-val" id="bcv">%BRCLK%</span></label>
<input type="range" name="brclk" min="0" max="15" value="%BRCLK%"
oninput="document.getElementById('bcv').textContent=this.value">
<label>News ticker brightness <span class="slider-val" id="bnv">%BRNWS%</span></label>
<input type="range" name="brnws" min="0" max="15" value="%BRNWS%"
oninput="document.getElementById('bnv').textContent=this.value">
<label>Aircraft display brightness <span class="slider-val" id="bav">%BRAIR%</span></label>
<input type="range" name="brair" min="0" max="15" value="%BRAIR%"
oninput="document.getElementById('bav').textContent=this.value">
<div class="hint">Each slider controls only the physical row showing that content.</div>
<hr style="border:none;border-top:1px solid #2a2d34;margin:14px 0">
<label style="display:flex;align-items:center">
<input type="checkbox" name="dnon" value="1" %DNCHECKED%>
Auto day/night dimming
</label>
<div class="hint">When on, the sliders above are your DAY brightness. Between sunset and sunrise everything dims to the night level below.</div>
<label>Night brightness <span class="slider-val" id="bnight">%BRNIGHT%</span></label>
<input type="range" name="brnight" min="0" max="15" value="%BRNIGHT%"
oninput="document.getElementById('bnight').textContent=this.value">
<button type="submit">Save</button>
</form></div>
)HTML";

const char CARD_SYSTEM[] PROGMEM = R"HTML(
<div class="card"><h2><span class="emoji">⚙️</span> System</h2>
<div class="stat-grid">
<div class="stat"><div class="k">Firmware</div><div class="v">%FWVER%</div></div>
<div class="stat"><div class="k">Reset reason</div><div class="v" id="reset-reason">%RESET_REASON%</div></div>
<div class="stat"><div class="k">Free heap</div><div class="v" id="heap">%HEAP%</div></div>
<div class="stat"><div class="k">WiFi RSSI</div><div class="v" id="rssi-stat">%RSSI_DB%</div></div>
</div>
<a class="link" href="/update">📦 Firmware OTA update →</a>
<a class="link" href="/logs">📋 Diagnostic logs →</a>
<div class="row">
<button class="ghost" onclick="fetch('/system/testpattern').then(()=>alert('Test pattern: all pixels on for 3s'))">🔲 Test pattern</button>
<button class="danger" onclick="if(confirm('Erase all settings and reboot? This cannot be undone.')){fetch('/system/factoryreset').then(()=>alert('Factory reset initiated. Device will reboot.'))}">️ Factory reset</button>
</div>
</div>
)HTML";

String formatUptime(unsigned long ms) {
  unsigned long s = ms / 1000;
  unsigned long d = s / 86400; s %= 86400;
  unsigned long h = s / 3600;  s %= 3600;
  unsigned long m = s / 60;    s %= 60;
  char buf[32];
  if (d > 0) snprintf(buf, sizeof(buf), "%lud %luh", d, h);
  else if (h > 0) snprintf(buf, sizeof(buf), "%luh %lum", h, m);
  else if (m > 0) snprintf(buf, sizeof(buf), "%lum %lus", m, s);
  else snprintf(buf, sizeof(buf), "%lus", s);
  return String(buf);
}

String resetReasonName(int code) {
  switch (code) {
    case 1:  return "Power-on";
    case 2:  return "SW restart";
    case 3:  return "Deep sleep wake";
    case 4:  return "Brownout";
    case 5:  return "SDIO reset";
    case 6:  return "Task WDT";
    case 7:  return "Other WDT";
    case 8:  return "Panic";
    case 9:  return "RTC WDT";
    case 10: return "Super WDT";
    case 11: return "RTC SW reset";
    case 12: return "RTC WDT reset";
    default: return String(code);
  }
}

String buildConfigPage() {
  String p;
  p.reserve(14000);   // whole page is ~11KB; reserve once so the many
                      // p += card concatenations below don't reallocate
                      // and recopy the growing buffer each time (that
                      // repeated realloc was a big part of the page's
                      // slowness to build and send).
  p = FPSTR(PAGE_HEAD);
  p.replace("%IP%", isAPMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString());
  p.replace("%FWVER%", FIRMWARE_VERSION);
  p.replace("%UPTIME%", formatUptime(millis()));
  int rssi = isAPMode ? 0 : WiFi.RSSI();
  int rssiPct;
  if (isAPMode) rssiPct = 0;
  else if (rssi <= -100) rssiPct = 5;
  else if (rssi >= -50) rssiPct = 100;
  else rssiPct = (rssi + 100) * 2;
  p.replace("%RSSI_PCT%", String(rssiPct));
  p.replace("%RSSI_DB%", isAPMode ? String("AP mode") : String(rssi) + " dBm");

  String c1 = FPSTR(CARD_NETWORK);
  c1.replace("%WIFI_SSID%", wifiSsid);
  c1.replace("%WIFI_PASS%", wifiPassword);
  p += c1;

  String c2 = FPSTR(CARD_CLOCK);
  c2.replace("%MASTERCHECKED%", masterClockMode ? "checked" : "");
  c2.replace("%BF0%", bigFontStyle == 0 ? "selected" : "");
  c2.replace("%BF1%", bigFontStyle == 1 ? "selected" : "");
  c2.replace("%BF4%", bigFontStyle == 4 ? "selected" : "");
  c2.replace("%OPT12%", use24h ? "" : "selected");
  c2.replace("%OPT24%", use24h ? "selected" : "");
  c2.replace("%OPTTOP%", clockOnTop ? "selected" : "");
  c2.replace("%OPTBOT%", clockOnTop ? "" : "selected");
  String opts = "";
  for (uint8_t i = 0; i < CITY_COUNT; i++) {
    opts += "<option value=\"" + String(i) + "\"" + (i == cityIndex ? " selected" : "") + ">" + String(CITY_LIST[i].name) + "</option>";
  }
  c2.replace("%CITY_OPTIONS%", opts);
  p += c2;

  String c3 = FPSTR(CARD_WEATHER);
  c3.replace("%WLAT%", String(weatherLat, 4));
  c3.replace("%WLON%", String(weatherLon, 4));
  String sunInfo;
  if (sunTimesValid) {
    char sb[120];
    snprintf(sb, sizeof(sb), "☀️ Sunrise <b style='color:var(--text)'>%02d:%02d</b> &nbsp;·&nbsp; 🌙 Sunset <b style='color:var(--text)'>%02d:%02d</b> &nbsp;·&nbsp; %s",
             sunriseMinutes / 60, sunriseMinutes % 60,
             sunsetMinutes / 60, sunsetMinutes % 60,
             isNightTime() ? "<span style='color:#60a5fa'>now: night</span>" : "<span style='color:#fbbf24'>now: day</span>");
    sunInfo = sb;
  } else {
    sunInfo = "Sun times not fetched yet — will populate after next weather update.";
  }
  c3.replace("%SUNINFO%", sunInfo);
  p += c3;

  String c4 = FPSTR(CARD_FEEDS);
  c4.replace("%NEWSCHECKED%", newsEnabled ? "checked" : "");
  c4.replace("%NEWSCYCLE%", String(newsCycleS));
  c4.replace("%NEWSSPD%", String(newsScrollSpeed));
  c4.replace("%NEWSSPACE%", String(newsLetterSpacing));
  c4.replace("%NEWSORD0%", newsOrderMode == 0 ? "checked" : "");
  c4.replace("%NEWSORD1%", newsOrderMode == 1 ? "checked" : "");
  String checkboxes = "";
  for (uint8_t i = 0; i < NEWS_SOURCE_COUNT; i++) {
    bool checked = newsSourceMask & (1 << i);
    checkboxes += "<div class='src-item'><input type='checkbox' name='src" + String(i) + "' value='1'" + (checked ? " checked" : "") + "><span>" + String(NEWS_SOURCES[i].name) + "</span></div>";
  }
  c4.replace("%NEWS_SOURCE_CHECKBOXES%", checkboxes);
  p += c4;

  String c5 = FPSTR(CARD_AIRCRAFT);
  c5.replace("%ACCHECKED%", aircraftEnabled ? "checked" : "");
  c5.replace("%ACHOST%", aircraftApiHost);
  c5.replace("%ACPORT%", String(aircraftApiPort));
  c5.replace("%ACSESS%", String(aircraftSessionSec));
  c5.replace("%ACFLD%", String(aircraftFieldSec));
  c5.replace("%ACFLD2%", String(aircraftFieldSec2));
  c5.replace("%ACBLANK%", String(aircraftBlankMs));
  c5.replace("%ACSCR%", String(aircraftScrollMs));
  c5.replace("%ACMAX%", String(aircraftMaxChars));
  p += c5;

  String c6 = FPSTR(CARD_APPEARANCE);
  c6.replace("%BRCLK%", String(brightnessClock));
  c6.replace("%BRNWS%", String(brightnessNews));
  c6.replace("%BRAIR%", String(brightnessAircraft));
  c6.replace("%DNCHECKED%", dayNightEnabled ? "checked" : "");
  c6.replace("%BRNIGHT%", String(nightBrightness));
  p += c6;

  String c7 = FPSTR(CARD_SYSTEM);
  c7.replace("%FWVER%", FIRMWARE_VERSION);
  c7.replace("%RESET_REASON%", resetReasonName((int)esp_reset_reason()));
  c7.replace("%HEAP%", String(ESP.getFreeHeap()) + " B");
  c7.replace("%RSSI_DB%", isAPMode ? String("AP mode") : String(WiFi.RSSI()) + " dBm");
  p += c7;

  p += FPSTR(PAGE_FOOT);
  return p;
}

void handleRoot() { server.send(200, "text/html", buildConfigPage()); }

void handleSaveNetwork() {
  if (server.hasArg("ssid")) wifiSsid = server.arg("ssid");
  if (server.hasArg("password")) wifiPassword = server.arg("password");
  prefs.begin("clock", false);
  prefs.putString("ssid", wifiSsid);
  prefs.putString("pass", wifiPassword);
  prefs.end();
  logMsg("Network saved — SSID: " + wifiSsid);
  server.sendHeader("Location", "/");
  server.send(303);
  delay(500);
  ESP.restart();
}

void handleSaveClock() {
  if (server.hasArg("use24h")) use24h = server.arg("use24h").toInt() == 1;
  if (server.hasArg("city")) {
    int c = server.arg("city").toInt();
    if (c >= 0 && c < CITY_COUNT) cityIndex = c;
  }
  if (server.hasArg("clocktop")) clockOnTop = server.arg("clocktop").toInt() == 1;
  masterClockMode = server.hasArg("masterclk") && server.arg("masterclk").toInt() == 1;
  if (server.hasArg("bigfont")) {
    int f = server.arg("bigfont").toInt();
    if (f >= 0 && f <= 4) bigFontStyle = f;
  }
  prefs.begin("clock", false);
  prefs.putBool("use24h", use24h);
  prefs.putUChar("city", cityIndex);
  prefs.putBool("clkTop", clockOnTop);
  prefs.putBool("masterClk", masterClockMode);
  prefs.putUChar("bigFont", bigFontStyle);
  prefs.end();
  logMsg("Clock saved — 24h=" + String(use24h) + " city=" + String(cityIndex) + " top=" + String(clockOnTop));
  if (timeSynced) syncTime();
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleSaveWeather() {
  if (server.hasArg("wlat")) weatherLat = server.arg("wlat").toFloat();
  if (server.hasArg("wlon")) weatherLon = server.arg("wlon").toFloat();
  prefs.begin("clock", false);
  prefs.putFloat("wLat", weatherLat);
  prefs.putFloat("wLon", weatherLon);
  prefs.end();
  logMsg("Weather saved — lat=" + String(weatherLat, 4) + " lon=" + String(weatherLon, 4));
  lastWeatherFetchMs = 0;
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleSaveFeeds() {
  newsEnabled = server.hasArg("newson") && server.arg("newson").toInt() == 1;
  uint16_t newsMask = 0;
  for (uint8_t i = 0; i < NEWS_SOURCE_COUNT; i++) {
    if (server.hasArg("src" + String(i))) newsMask |= (1 << i);
  }
  newsSourceMask = newsMask;
  if (server.hasArg("newscycle")) newsCycleS = constrain(server.arg("newscycle").toInt(), 8, 120);
  if (server.hasArg("newsspd")) newsScrollSpeed = constrain(server.arg("newsspd").toInt(), 1, 10);
  if (server.hasArg("newsspace")) newsLetterSpacing = constrain(server.arg("newsspace").toInt(), 1, 5);
  uint8_t ord = 0;
  if (server.hasArg("newsord")) ord = constrain(server.arg("newsord").toInt(), 0, 1);
  newsOrderMode = ord;
  newsShownMask = 0;
  prefs.begin("clock", false);
  prefs.putBool("newsOn", newsEnabled);
  prefs.putUShort("newsMask", newsSourceMask);
  prefs.putUShort("newsCycS", newsCycleS);
  prefs.putUChar("newsSpd", newsScrollSpeed);
  prefs.putUChar("newsSpc", newsLetterSpacing);
  prefs.putUChar("newsOrd", newsOrderMode);
  prefs.end();
  logMsg("Feeds saved — on=" + String(newsEnabled) + " mask=" + String(newsSourceMask) + " ord=" + String(newsOrderMode));
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleSaveAircraft() {
  aircraftEnabled = server.hasArg("acon") && server.arg("acon").toInt() == 1;
  if (server.hasArg("achost")) aircraftApiHost = server.arg("achost");
  if (server.hasArg("acport")) aircraftApiPort = constrain(server.arg("acport").toInt(), 1, 65535);
  if (server.hasArg("acsess")) aircraftSessionSec = constrain(server.arg("acsess").toInt(), 0, 600);
  if (server.hasArg("acfld"))  aircraftFieldSec   = constrain(server.arg("acfld").toInt(), 1, 60);
  if (server.hasArg("acfld2")) aircraftFieldSec2  = constrain(server.arg("acfld2").toInt(), 1, 60);
  if (server.hasArg("acblank")) aircraftBlankMs   = constrain(server.arg("acblank").toInt(), 0, 5000);
  if (server.hasArg("acscr"))  aircraftScrollMs   = constrain(server.arg("acscr").toInt(), 5, 200);
  if (server.hasArg("acmax"))  aircraftMaxChars   = constrain(server.arg("acmax").toInt(), 4, 30);
  prefs.begin("clock", false);
  prefs.putBool("acOn", aircraftEnabled);
  prefs.putString("acHost", aircraftApiHost);
  prefs.putUShort("acPort", aircraftApiPort);
  prefs.putUShort("acSess", aircraftSessionSec);
  prefs.putUShort("acFld", aircraftFieldSec);
  prefs.putUShort("acFld2", aircraftFieldSec2);
  prefs.putUShort("acBlank", aircraftBlankMs);
  prefs.putUChar("acScr", aircraftScrollMs);
  prefs.putUChar("acMax", aircraftMaxChars);
  prefs.end();
  logMsg("Aircraft saved — on=" + String(aircraftEnabled) + " host=" + aircraftApiHost);
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleSaveAppearance() {
  if (server.hasArg("brclk")) brightnessClock = constrain(server.arg("brclk").toInt(), 0, 15);
  if (server.hasArg("brnws")) brightnessNews = constrain(server.arg("brnws").toInt(), 0, 15);
  if (server.hasArg("brair")) brightnessAircraft = constrain(server.arg("brair").toInt(), 0, 15);
  dayNightEnabled = server.hasArg("dnon") && server.arg("dnon").toInt() == 1;
  if (server.hasArg("brnight")) nightBrightness = constrain(server.arg("brnight").toInt(), 0, 15);
  prefs.begin("clock", false);
  prefs.putUChar("brClk", brightnessClock);
  prefs.putUChar("brNws", brightnessNews);
  prefs.putUChar("brAir", brightnessAircraft);
  prefs.putBool("dnOn", dayNightEnabled);
  prefs.putUChar("brNight", nightBrightness);
  prefs.end();
  lastAppliedTopBrightness = 255;
  lastAppliedBotBrightness = 255;
  logMsg("Appearance saved — day/night=" + String(dayNightEnabled));
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleSystemStatus() {
  String json = "{";
  json += "\"fw\":\"" + String(FIRMWARE_VERSION) + "\",";
  json += "\"uptime\":" + String(millis() / 1000) + ",";
  json += "\"heap\":" + String(ESP.getFreeHeap()) + ",";
  json += "\"reset\":" + String((int)esp_reset_reason()) + ",";
  json += "\"rssi\":" + String(isAPMode ? 0 : WiFi.RSSI()) + ",";
  json += "\"ap\":" + String(isAPMode ? 1 : 0);
  json += "}";
  server.send(200, "application/json", json);
}

void handleFactoryReset() {
  logMsg("FACTORY RESET — erasing NVS and rebooting");
  prefs.begin("clock", false);
  prefs.clear();
  prefs.end();
  server.send(200, "text/plain", "OK — rebooting");
  delay(500);
  ESP.restart();
}

void handleTestPattern() {
  testPatternActive = true;
  testPatternUntilMs = millis() + 3000;
  logMsg("Test pattern: all pixels on for 3s");
  server.send(200, "text/plain", "OK");
}

const char UPDATE_PAGE[] PROGMEM = R"HTML(
<!DOCTYPE html><html><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>OTA</title><style>body{font-family:-apple-system,sans-serif;max-width:420px;margin:20px auto;padding:0 16px;background:#0b0c0f;color:#eee}h2{color:#4ade80;margin-bottom:4px}.sub{color:#888;font-size:13px;margin-top:0}input[type=file]{width:100%;padding:10px;background:#1e2025;border:1px solid #2a2d34;border-radius:6px;color:#eee;margin:12px 0}button{margin-top:8px;padding:10px 16px;background:#4ade80;border:none;border-radius:6px;font-weight:600;cursor:pointer;width:100%;color:#08110b}a{color:#4ade80;text-decoration:none;display:block;text-align:center;margin-top:16px}</style>
</head><body>
<a href="/">&larr; Back to console</a>
<h2>📦 Firmware OTA Update</h2>
<p class="sub">Upload a .bin firmware file. The device will reboot automatically after flashing.</p>
<form method="POST" action="/doupdate" enctype="multipart/form-data">
<input type="file" name="firmware" accept=".bin" required>
<button type="submit">Upload &amp; Flash</button>
</form>
</body></html>
)HTML";

void handleUpdatePage() { server.send(200, "text/html", FPSTR(UPDATE_PAGE)); }

void handleDoUpdateResult() {
  bool ok = !Update.hasError();
  server.send(200, "text/html", String("<body style='background:#111;color:#eee;font-family:sans-serif;text-align:center;padding:40px'><p style='font-size:18px'>") + (ok ? "✅ Update OK. Rebooting..." : "❌ Update FAILED.") + "</p></body>");
  if (ok) { delay(500); ESP.restart(); }
}

void handleDoUpdateUpload() {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    esp_task_wdt_delete(NULL);
    logMsg("OTA start — watchdog paused for upload");
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
  }
  else if (up.status == UPLOAD_FILE_WRITE) {
    esp_task_wdt_reset();
    if (Update.write(up.buf, up.currentSize) != up.currentSize) Update.printError(Serial);
  }
  else if (up.status == UPLOAD_FILE_END) {
    if (!Update.end(true)) Update.printError(Serial);
    esp_task_wdt_add(NULL);
    logMsg("OTA end — watchdog re-armed");
  }
}

const char LOGS_PAGE[] PROGMEM = R"HTML(
<!DOCTYPE html><html><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Logs</title><style>body{font-family:-apple-system,sans-serif;max-width:720px;margin:0 auto;padding:20px 16px;background:#0b0c0f;color:#eee}pre{background:#16181d;border:1px solid #2a2d34;border-radius:8px;padding:12px;font-size:12px;white-space:pre-wrap;max-height:70vh;overflow-y:auto;font-family:ui-monospace,monospace}a{color:#4ade80;text-decoration:none}h1{color:#4ade80;font-size:18px;margin:0 0 12px}</style>
</head><body>
<a href="/">&larr; Back to console</a>
<h1>📋 Diagnostic Logs</h1>
<pre id="lb">%LOGS%</pre>
<script>setInterval(async()=>{try{const t=await(await fetch('/logs.txt')).text();document.getElementById('lb').textContent=t;}catch(e){}},3000);</script>
</body></html>
)HTML";

void handleLogsPage() {
  String p = FPSTR(LOGS_PAGE);
  p.replace("%LOGS%", getLogsAsText());
  server.send(200, "text/html", p);
}
void handleLogsRaw() { server.send(200, "text/plain", getLogsAsText()); }

void setupWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save/network",   HTTP_POST, handleSaveNetwork);
  server.on("/save/clock",     HTTP_POST, handleSaveClock);
  server.on("/save/weather",   HTTP_POST, handleSaveWeather);
  server.on("/save/feeds",     HTTP_POST, handleSaveFeeds);
  server.on("/save/aircraft",  HTTP_POST, handleSaveAircraft);
  server.on("/save/appearance", HTTP_POST, handleSaveAppearance);
  server.on("/system/status",       HTTP_GET, handleSystemStatus);
  server.on("/system/factoryreset", HTTP_GET, handleFactoryReset);
  server.on("/system/testpattern",  HTTP_GET, handleTestPattern);
  server.on("/update",    HTTP_GET, handleUpdatePage);
  server.on("/doupdate",  HTTP_POST, handleDoUpdateResult, handleDoUpdateUpload);
  server.on("/logs",      HTTP_GET, handleLogsPage);
  server.on("/logs.txt",  HTTP_GET, handleLogsRaw);
  server.begin();
}

// ============================================================================
// SETUP + LOOP
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(200);

  // Hardware Task Watchdog: reboots on a true hang (15s) instead of
  // needing a manual power cycle. Guarded for both ESP32 core 2.x and 3.x.
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t wdtCfg = { .timeout_ms = 15000, .idle_core_mask = 0, .trigger_panic = true };
  esp_task_wdt_reconfigure(&wdtCfg);
#else
  esp_task_wdt_init(15, true);
#endif
  esp_task_wdt_add(NULL);

  loadConfig();
  logMsg("Boot " FIRMWARE_VERSION);
  // Reset reason helps diagnose the reboot loop: 1=power on, 3=software
  // restart (ESP.restart), 6=watchdog, 15=brownout, others exist too.
  logMsg("Reset reason: " + String((int)esp_reset_reason()) + ", free heap: " + String(ESP.getFreeHeap()));
  // Log every persisted config value so /logs immediately shows what
  // the sketch actually loaded from NVS — no more guessing whether a
  // slider "took effect."
  logMsg("Config: newsSpd=" + String(newsScrollSpeed) +
         " newsCycS=" + String(newsCycleS) +
         " newsMask=" + String(newsSourceMask) +
         " newsOn=" + String(newsEnabled));
  logMsg("Config: acScr=" + String(aircraftScrollMs) +
         " acSess=" + String(aircraftSessionSec) +
         " acFld=" + String(aircraftFieldSec) +
         " acFld2=" + String(aircraftFieldSec2) +
         " acMax=" + String(aircraftMaxChars));
  logMsg("Config: brClk=" + String(brightnessClock) +
         " brNws=" + String(brightnessNews) +
         " brAir=" + String(brightnessAircraft));

  mx.begin();
  mx.control(MD_MAX72XX::TEST, MD_MAX72XX::OFF);
  mx.control(MD_MAX72XX::INTENSITY, brightness);
  // Disable auto-update: without this, EVERY individual setRow() call
  // (128 of them per frame — 16 devices x 8 rows) triggers its own SPI
  // transfer. With it off, setRow just updates the in-memory buffer,
  // and the single mx.update() call at the end of fbFlush() pushes
  // everything to hardware in one batch. This should noticeably reduce
  // per-frame SPI overhead and may be contributing to jerkiness.
  mx.control(MD_MAX72XX::UPDATE, MD_MAX72XX::OFF);
  mx.clear();

  // Splash: show "ESP32 Clock" / version for 2 full seconds. Guaranteed
  // visible regardless of what fetches happen after. Previously v17
  // showed splash for 1.5s but then fetchAircraft() could run
  // immediately after WiFi came up (within another 1s), so if a plane
  // was overhead the boot experience was "flash of splash then aircraft
  // text before you'd even seen the version." Now we don't fetch
  // aircraft until at least 5s after boot, giving the user time to
  // read the splash and connecting-message.
  fbClear();
  drawStringCentered(0, 64, 1, "LED Clock");
  drawStringCentered(0, 64, 9, FIRMWARE_VERSION);
  fbFlush();
  delay(2000);

  fbClear();
  drawStringCentered(0, 64, 1, "Connecting");
  {
    char wl[40];
    snprintf(wl, sizeof(wl), "to %s", wifiSsid.c_str());
    if (stringWidthPx(wl) <= 64) drawStringCentered(0, 64, 9, wl);
    else drawStringCentered(0, 64, 9, wifiSsid.c_str());
  }
  fbFlush();

  connectWiFi();
  esp_task_wdt_reset();
  // Start the web server FIRST — right after WiFi is up — so the config
  // page is reachable immediately. Previously it started AFTER syncTime()
  // + fetchWeather(), which could take several seconds; opening the page
  // during that window failed (the "web won't open after reset until a
  // few reboots" symptom). Time sync and the first weather fetch now
  // happen after the server is already listening.
  if (!isAPMode) {
    ArduinoOTA.setHostname("esp32-1664-clock");
    ArduinoOTA.onStart([]() { esp_task_wdt_delete(NULL); logMsg("ArduinoOTA start — WDT paused"); });
    ArduinoOTA.onError([](ota_error_t e) { esp_task_wdt_add(NULL); });
    ArduinoOTA.begin();
  }
  setupWebServer();
  esp_task_wdt_reset();

  if (!isAPMode) {
    syncTime();
    esp_task_wdt_reset();
    fetchWeather();
    esp_task_wdt_reset();
    lastWeatherFetchMs = millis();
    lastAircraftPollMs = millis();
  }
  refreshTimeCache();
  render();
}

void loop() {
  esp_task_wdt_reset();   // feed the hardware watchdog every loop iteration

  // Software watchdog: track when loop last ran. If we're wedged for
  // more than 30s (a network stack hang, an SPI stall, etc.) force a
  // clean restart rather than letting the user power cycle. Uses
  // static-init so it starts fresh at each boot.
  static unsigned long lastLoopHeartbeatMs = 0;
  if (lastLoopHeartbeatMs == 0) lastLoopHeartbeatMs = millis();
  // If we've come back from a >30s stall, log it before doing anything else
  if (millis() - lastLoopHeartbeatMs > 30000) {
    logMsg("STALL: loop was blocked for " + String((millis() - lastLoopHeartbeatMs)/1000) + "s — restarting");
    delay(500);
    ESP.restart();
  }
  lastLoopHeartbeatMs = millis();

  // Loop stats: rate and longest iteration. Logged every 60s so /logs
  // can show whether the main loop is running smoothly or getting
  // starved by long blocking calls (fetches, TLS handshakes).
  static unsigned long loopIterations = 0;
  static unsigned long longestIterationMs = 0;
  static unsigned long lastLoopStartMs = 0;
  static unsigned long lastStatsLogMs = 0;
  unsigned long loopStartMs = millis();
  if (lastLoopStartMs > 0) {
    unsigned long thisIterMs = loopStartMs - lastLoopStartMs;
    if (thisIterMs > longestIterationMs) longestIterationMs = thisIterMs;
  }
  lastLoopStartMs = loopStartMs;
  loopIterations++;

  if (millis() - lastStatsLogMs >= 60000) {
    unsigned long elapsedSec = (millis() - lastStatsLogMs) / 1000;
    if (elapsedSec == 0) elapsedSec = 1;
    unsigned long loopsPerSec = loopIterations / elapsedSec;
    logMsg("Stats: uptime=" + String(millis() / 1000) + "s" +
           " loops/s=" + String(loopsPerSec) +
           " maxIterMs=" + String(longestIterationMs) +
           " heap=" + String(ESP.getFreeHeap()));
    loopIterations = 0;
    longestIterationMs = 0;
    lastStatsLogMs = millis();
  }
  if (!isAPMode) ArduinoOTA.handle();
  server.handleClient();

  if (!isAPMode) {
    if (WiFi.status() != WL_CONNECTED) connectWiFi();
    if (!timeSynced) syncTime();
    refreshTimeCache();
  }

  // 1-second tick: colon blink
  static unsigned long lastTickMs = 0;
  if (millis() - lastTickMs >= 1000) {
    lastTickMs = millis();
    colonOn = !colonOn;
  }

  // Icon animation: cycle 4 frames every 250ms — visibly smoother than
  // the old 2-frame swap. Marks render dirty when the frame changes.
  static unsigned long lastIconAnimMs = 0;
  static bool iconAnimDirty = false;
  // No weather icon in master clock mode, so don't animate (would force
  // 4 needless full re-renders/sec that compete with server.handleClient).
  if (!masterClockMode && millis() - lastIconAnimMs >= 250) {
    lastIconAnimMs = millis();
    iconFrame = (iconFrame + 1) & 3;
    iconAnimDirty = true;
  }

  // News cycle: alternate date/news every newsCycleS.
  //
  // PRE-FETCH DESIGN: we do NOT fetch at flip-in. Fetching at flip-in
  // means the scroll freezes for up to 5s (fetch timeout) right when
  // the user is trying to read the just-started headline. Instead, we
  // fetch ~3s before the flip, while the date is still on screen and
  // static — the block is invisible then (no scroll to freeze), and
  // by the time the flip happens, the headline is already loaded.
  static bool newsPrefetched = false;
  static unsigned long newsPhaseDurationMs = 0;
  unsigned long newsScrollStepMs = 52 - (((unsigned long)(newsScrollSpeed - 1) * 21) / 9);
  if (!isAPMode && newsEnabled && !masterClockMode) {
    unsigned long cycleMs = (unsigned long)newsCycleS * 1000UL;
    unsigned long inThisPhase = millis() - lastDateNewsFlipMs;
    // Date phase = fixed cycleMs; news phase = its own computed one-pass
    // duration (capped at cycleMs).
    unsigned long thisPhaseDurationMs = showingNews ? newsPhaseDurationMs : cycleMs;

    // Pre-fetch at the START of the date phase (600ms in), not near its
    // end. This gives the fetch the ENTIRE clock-display window to finish,
    // so the headline is always in hand before the news phase begins and
    // the scroll never waits on a blocking fetch (no stutter).
    if (!showingNews && !newsPrefetched && inThisPhase >= 600) {
      int idx = nextNewsSource();
      if (idx >= 0) {
        bool ok = fetchNewsFromSource((uint8_t)idx);
        if (!ok) newsAvailable = false;   // failure — flip-in will show date instead
      }
      newsPrefetched = true;
    }

    // Flip
    if (inThisPhase >= thisPhaseDurationMs) {
      lastDateNewsFlipMs = millis();
      showingNews = !showingNews;
      scrollOffsetPx = 0;
      newsPrefetched = false;   // reset for the next date phase
      if (showingNews && !newsAvailable) showingNews = false;   // no headline to show
      if (showingNews) {
        // News phase = min(one full scroll pass, newsCycleS). Short
        // headline finishes early and returns; long one cut off at the
        // cap. Scrolls exactly once either way, never repeats.
        int headlineWidthPx = stringWidthPxSpaced(newsHeadline, newsLetterSpacing - 2);
        unsigned long onePassMs = (unsigned long)(headlineWidthPx + 64) * newsScrollStepMs;
        newsPhaseDurationMs = (onePassMs < cycleMs) ? onePassMs : cycleMs;
        if (newsPhaseDurationMs < 2000) newsPhaseDurationMs = 2000;
      }
    }
  } else {
    showingNews = false;
    newsPrefetched = false;
  }

  // Aircraft polling — every 2s, only if enabled
  if (!isAPMode && aircraftEnabled && !masterClockMode && millis() - lastAircraftPollMs >= aircraftPollIntervalMs) {
    lastAircraftPollMs = millis();
    fetchAircraft();
  }

  // Aircraft field state machine: while aircraftActive, rotate through
  // fields with blank pauses. Session timeout is checked ONLY at phase
  // transitions (not mid-phase), and only AFTER phase 1 (Departure/
  // Arrival) has completed at least once — this guarantees the user
  // always sees at least one full carrier/type + departure/arrival
  // cycle before the display returns to the clock, even with short
  // session settings like acSess=10s.
  bool aircraftActive = aircraftEnabled && aircraftPresent && !aircraftReturnedToClock;
  if (aircraftActive) {
    unsigned long inThisPhase = millis() - aircraftFieldStartedMs;
    // Base configured duration for this phase.
    unsigned long configuredMs =
      (aircraftFieldIdx == 0 ? (unsigned long)aircraftFieldSec : (unsigned long)aircraftFieldSec2) * 1000UL;
    // Scroll-aware duration: figure out the two texts on this phase and,
    // if either is long enough to scroll, make the phase last exactly one
    // scroll pass. This fixes the "text scrolls halfway, disappears, and
    // restarts" bug — that happened because a fixed field timer let the
    // scroll wrap around (% totalRange) and restart mid-phase. Now the
    // phase ends right as the single pass completes, so it never wraps.
    const char* topT; const char* botT;
    if (aircraftFieldIdx == 0) { topT = aircraftCarrier; botT = aircraftType; }
    else                       { topT = aircraftOrigin;  botT = aircraftDest; }
    int wTop = stringWidthPx(topT);
    int wBot = stringWidthPx(botT);
    int widest = wTop > wBot ? wTop : wBot;
    unsigned long thisPhaseDurationMs;
    if (widest > 64) {
      // Scrolls: one pass = (textWidth + display) * ms-per-column.
      unsigned long onePassMs = (unsigned long)(widest + 64) * aircraftScrollMs;
      thisPhaseDurationMs = onePassMs;
      // But never shorter than the configured minimum (so a barely-over
      // text isn't a blink) and give a small tail so the end is readable.
      if (thisPhaseDurationMs < configuredMs) thisPhaseDurationMs = configuredMs;
    } else {
      // Fits statically — hold for the configured duration.
      thisPhaseDurationMs = configuredMs;
    }
    if (!aircraftFieldBlank && inThisPhase >= thisPhaseDurationMs) {
      // Field visible period ended — enter blank
      aircraftFieldBlank = true;
      aircraftFieldStartedMs = millis();
    } else if (aircraftFieldBlank && inThisPhase >= aircraftBlankMs) {
      // Blank ended — advance to next phase (Phase 0 -> Phase 1 -> loop)
      bool haveRoute = (aircraftOrigin[0] != 0) || (aircraftDest[0] != 0);
      uint8_t prevPhase = aircraftFieldIdx;
      aircraftFieldIdx = (aircraftFieldIdx + 1) % 2;
      // Skip phase 1 if we don't have route info.
      if (!haveRoute && aircraftFieldIdx == 1) aircraftFieldIdx = 0;

      // Decide whether this transition should end the session. We only
      // check the session timer at transitions where we're about to
      // loop back to phase 0 — that means either phase 1 just finished
      // (both got shown at least once), or we're skipping phase 1 due
      // to no route (phase 0 got shown at least once).
      bool aboutToLoop = (prevPhase == 1) || (!haveRoute && prevPhase == 0);
      if (aboutToLoop && aircraftSessionSec > 0 &&
          millis() - aircraftSessionStartedMs >= (unsigned long)aircraftSessionSec * 1000UL) {
        aircraftReturnedToClock = true;
        logMsg("Aircraft session complete — back to clock");
      } else {
        aircraftFieldBlank = false;
        aircraftFieldStartedMs = millis();
        scrollOffsetPx = 0;
        scrollStartMs = millis();
      }
    }
  }
  if (aircraftEnabled && !aircraftPresent) {
    // Aircraft has left — clear the returned-to-clock latch so the next
    // aircraft gets a fresh session.
    aircraftReturnedToClock = false;
  }

  // Scroll animation tick
  bool isScrolling = (aircraftActive && !aircraftFieldBlank) ||
                     (newsEnabled && showingNews && newsAvailable);
  // Scroll pace: aircraft uses its own configured scroll speed; news
  // uses a 1..10 slider. Narrowed to the range people actually use —
  // the old 80ms(slow)..8ms(fast) span meant speeds 1-3 were painfully
  // slow and 8-10 were an unreadable blur; nobody used the extremes.
  // Now the full 1..10 slider maps across 52ms(slowest useful)..
  // 31ms(fastest useful) per column, giving finer control within the
  // range that's actually readable and used.
  unsigned long scrollStepMs;
  if (aircraftActive) {
    scrollStepMs = aircraftScrollMs;
  } else {
    scrollStepMs = newsScrollStepMs;   // computed once in the news-flip block above
  }
  // ============================================================================
  // SCROLL TIMING — rewritten to absolute-time positioning
  //
  // v17/v18 used an ACCUMULATING approach: each iteration added N steps
  // to scrollOffsetPx based on elapsed time, clamped to avoid big jumps.
  // The clamp was the actual bug: your stats showed loop iterations
  // spiking to 400-1200ms (from blocking fetches). At that spike, the
  // real elapsed time demanded e.g. 20+ pixel-steps, but the clamp
  // capped it at 4 — so the scroll fell behind and STAYED behind,
  // re-triggering the clamp on every subsequent frame near a fetch.
  // That's a sustained stutter, not a one-time hiccup — clamping an
  // accumulator creates permanent drift.
  //
  // Fixed by computing position directly from elapsed wall-clock time:
  //   scrollOffsetPx = (millis() - scrollStartMs) / scrollStepMs
  // No accumulation, no drift possible. A loop spike just means one
  // frame jumps further than usual (visually: a brief hop), then
  // continues exactly on the correct time-based schedule — it can't
  // fall behind and stay behind, because position is recomputed fresh
  // every time from an absolute clock, not from the previous position.
  // ============================================================================
  static bool wasScrolling = false;
  if (isScrolling && !wasScrolling) {
    scrollStartMs = millis();
    scrollOffsetPx = 0;
  }
  wasScrolling = isScrolling;

  int newScrollOffsetPx = scrollOffsetPx;
  if (isScrolling) {
    newScrollOffsetPx = (int)((millis() - scrollStartMs) / scrollStepMs);
  }
  bool scrollAdvanced = (newScrollOffsetPx != scrollOffsetPx);
  scrollOffsetPx = newScrollOffsetPx;

  // Render trigger: scroll advance forces a render, icon animation
  // forces a render, otherwise once per second is enough for the clock
  // colon blink. Note: NO independent scroll-render timer any more —
  // if the scroll didn't advance, there's nothing new to draw.
  static unsigned long lastRenderMs = 0;
  static unsigned long maxRenderUs = 0;
  static unsigned long renderCount = 0;
  // Test pattern expiry check (in loop, so the end is detected promptly
  // regardless of render timing). testPatternActive itself is cleared in
  // render() when it draws the final frame.
  static bool testPatternWasActive = false;
  bool testPatternRunning = testPatternActive && (millis() < testPatternUntilMs);
  bool testPatternJustStarted = (testPatternActive && !testPatternWasActive);
  bool testPatternJustEnded = (testPatternWasActive && !testPatternRunning);
  testPatternWasActive = testPatternActive;

  bool timeToRender = false;
  if (scrollAdvanced) timeToRender = true;
  else if (iconAnimDirty) { timeToRender = true; iconAnimDirty = false; }
  else if (testPatternJustStarted || testPatternJustEnded) timeToRender = true;
  else if (millis() - lastRenderMs >= 1000) timeToRender = true;
  if (timeToRender) {
    lastRenderMs = millis();
    unsigned long renderStartUs = micros();
    render();
    unsigned long renderUs = micros() - renderStartUs;
    if (renderUs > maxRenderUs) maxRenderUs = renderUs;
    renderCount++;
  }

  // Render timing stats — separate from the general loop stats, to
  // isolate whether render()/fbFlush() (the SPI write to the matrix)
  // is itself a source of frame-time variance, independent of network
  // blocking. Logged every 20s (more frequent than the 60s general
  // stats) since scroll issues are easier to correlate at this grain.
  static unsigned long lastRenderStatsMs = 0;
  if (millis() - lastRenderStatsMs >= 20000) {
    lastRenderStatsMs = millis();
    logMsg("RenderStats: count=" + String(renderCount) +
           " maxRenderUs=" + String(maxRenderUs) +
           " scrollStepMs=" + String(scrollStepMs));
    maxRenderUs = 0;
    renderCount = 0;
  }

  if (!isAPMode && millis() - lastWeatherFetchMs >= WEATHER_FETCH_INTERVAL_MS) {
    lastWeatherFetchMs = millis();
    fetchWeather();
  }
}