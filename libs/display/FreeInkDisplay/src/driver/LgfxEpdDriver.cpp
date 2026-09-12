#include "LgfxEpdDriver.h"

#include <BoardConfig.h>

#include <cmath>
#include <cstring>

#if FREEINK_DRIVER_LGFX_EPD
#include <M5GFX.h>  // pulls LovyanGFX; added to lib_deps only on the LilyGo env
#include <esp_heap_caps.h>
#include <lgfx/v1/platforms/esp32/Bus_EPD.h>
#include <lgfx/v1/platforms/esp32/Panel_EPD.hpp>
#endif

namespace freeink {

#if FREEINK_DRIVER_LGFX_EPD
namespace {

// Set from the active config in begin(), read by the bus subclass below. The
// driver is a singleton (one panel), so a file-scope pointer is fine and mirrors
// how M5GFX/LovyanGFX use global device objects.
const LgfxEpdPowerHooks* g_hooks = nullptr;

// Bus subclass that defers the board's power topology to injected hooks. Matches
// the two override points LovyanGFX exposes: init() (pin setup) and
// powerControl() (rail up/down), guarding the _pwr_on state itself. A board
// whose rails are plain GPIOs (pinOe/pinPwr/pinSpv, e.g. M5Stack PaperS3) leaves
// the corresponding hook null and gets Bus_EPD's stock power sequence instead;
// a board with external power silicon (LilyGo's TPS65185 + PCA9535) hooks it.
class FreeInkBusEPD : public lgfx::Bus_EPD {
 public:
  bool init() override {
    if (g_hooks && g_hooks->prepare && !g_hooks->prepare()) return false;
    return lgfx::Bus_EPD::init();
  }

  bool powerControl(const bool powerOn) override {
    // _pwr_known guards the short-circuit below. Without it a hook failure was
    // unrecoverable: _pwr_on was assigned whether or not the rails actually
    // moved, so one failed transition made the cached state a lie that no later
    // call could correct -- every subsequent request for that same state
    // returned early and never re-issued the hook. On failure the rails are in
    // an unknown position, so neither direction may short-circuit.
    if (_pwr_known && _pwr_on == powerOn) return true;
    const bool hooked = g_hooks && (powerOn ? g_hooks->powerOn != nullptr : g_hooks->powerOff != nullptr);
    if (!hooked) return lgfx::Bus_EPD::powerControl(powerOn);
    wait();
    if (powerOn) {
      if (!g_hooks->powerOn()) {
        // The board attempts its own power-off cleanup, but it is unverified.
        _pwr_known = false;
        return false;
      }
      _pwr_on = true;
      _pwr_known = true;
      return true;
    }
    const bool ok = g_hooks->powerOff();
    _pwr_on = false;
    _pwr_known = ok;
    return ok;
  }

 private:
  // Starts false: the rails' position at construction is genuinely unknown, so
  // the first transition always reaches the hook.
  bool _pwr_known = false;
};

class FreeInkLgfxEpd : public lgfx::LGFX_Device {
 public:
  void setup(const LgfxEpdConfig& c, uint16_t w, uint16_t h) {
    auto bc = _bus.config();
    bc.bus_speed = c.busHz;
    for (int i = 0; i < 8; ++i) bc.pin_data[i] = c.dataPins[i];
    bc.pin_pwr = c.pinPwr;
    bc.pin_sph = c.pinSph;
    bc.pin_spv = c.pinSpv;
    bc.pin_oe = c.pinOe;
    bc.pin_le = c.pinLe;
    bc.pin_cl = c.pinCl;
    bc.pin_ckv = c.pinCkv;
    bc.bus_width = 8;
    _bus.config(bc);

    _panel.setBus(&_bus);

    auto dc = _panel.config_detail();
    dc.line_padding = c.linePadding;
    if (c.lutQuality && c.lutQualityStep) {
      dc.lut_quality = c.lutQuality;
      dc.lut_quality_step = c.lutQualityStep;
    }
    if (c.lutText && c.lutTextStep) {
      dc.lut_text = c.lutText;
      dc.lut_text_step = c.lutTextStep;
    }
    if (c.lutFast && c.lutFastStep) {
      dc.lut_fast = c.lutFast;
      dc.lut_fast_step = c.lutFastStep;
    }
    if (c.lutFastest && c.lutFastestStep) {
      dc.lut_fastest = c.lutFastest;
      dc.lut_fastest_step = c.lutFastestStep;
    }
    _panel.config_detail(dc);

    auto pc = _panel.config();
    pc.memory_width = pc.panel_width = w;
    pc.memory_height = pc.panel_height = h;
    pc.offset_rotation = 0;
    pc.offset_x = 0;
    pc.offset_y = 0;
    pc.bus_shared = false;
    _panel.config(pc);

    setPanel(&_panel);
  }

 private:
  FreeInkBusEPD _bus;
  lgfx::Panel_EPD _panel;
};

FreeInkLgfxEpd g_dev;

lgfx::epd_mode::epd_mode_t epdModeFor(RefreshMode m) {
  switch (m) {
    case RefreshMode::Full: return lgfx::epd_mode::epd_text;
    case RefreshMode::Half: return lgfx::epd_mode::epd_text;
    default: return lgfx::epd_mode::epd_fast;
  }
}

// 8-bit gray canvas (PSRAM) the panel pushes from, plus the two 1-bpp planes the
// facade streams for grayscale. Allocated once in begin().
lgfx::LGFX_Sprite* g_canvas = nullptr;
uint8_t* g_lsb = nullptr;
uint8_t* g_msb = nullptr;
uint16_t g_w = 0, g_h = 0, g_wb = 0;

// 0x00 and 0xFF survive Panel_EPD's quantiser at both rails whatever the Bayer
// cell (it clamps), so the two rails need no board input. The greys do -- see
// LgfxEpdConfig::grayDark.
constexpr uint8_t kGrayBlack = 0x00, kGrayWhite = 0xFF;
uint8_t g_grayDark = 0, g_grayLight = 0;

void allocCanvas(uint16_t w, uint16_t h) {
  g_w = w;
  g_h = h;
  g_wb = w / 8;
  if (!g_canvas) {
    g_canvas = new lgfx::LGFX_Sprite(&g_dev);
    g_canvas->setPsram(true);
    g_canvas->setColorDepth(lgfx::color_depth_t::grayscale_8bit);
    g_canvas->createSprite(w, h);
  }
  const size_t planeBytes = static_cast<size_t>(g_wb) * h;
  if (!g_lsb) g_lsb = static_cast<uint8_t*>(heap_caps_malloc(planeBytes, MALLOC_CAP_SPIRAM));
  if (!g_msb) g_msb = static_cast<uint8_t*>(heap_caps_malloc(planeBytes, MALLOC_CAP_SPIRAM));
}

// Expand a 1-bpp B/W frame (bit set = white) into the 8-bit gray canvas.
void fillCanvasBW(const uint8_t* fb) {
  if (!g_canvas) return;
  auto* dst = static_cast<uint8_t*>(g_canvas->getBuffer());
  if (!dst) return;
  for (uint16_t y = 0; y < g_h; ++y) {
    const uint8_t* src = fb + static_cast<uint32_t>(y) * g_wb;
    uint8_t* drow = dst + static_cast<uint32_t>(y) * g_w;
    for (uint16_t bx = 0; bx < g_wb; ++bx) {
      const uint8_t b = src[bx];
      for (uint8_t bit = 0; bit < 8; ++bit) drow[bx * 8 + bit] = (b & (0x80 >> bit)) ? kGrayWhite : kGrayBlack;
    }
  }
}

// Overlay the buffered LSB/MSB planes onto the B/W canvas the base push left
// behind, darkening only the pixels a plane actually selects.
//
// This used to take the base frame as an argument and rebuild every pixel from
// it. That looked reasonable but could not work: displayGray() is handed
// FreeInkDisplay::frameBuffer, and the host's plane dance (clear to 0x00, render
// text-only, copy the plane out, call displayGray) leaves the LAST PLANE there,
// not the page. Ssd1677Driver::displayGray() opens with `(void)fb` -- its planes
// are already in controller RAM and the panel retains the B/W image -- so nothing
// ever noticed that the buffer held a plane. Here it painted the whole background
// black and left only the anti-aliased marks standing.
//
// The canvas already holds the B/W frame from the base push and is not cleared by
// pushSprite(), so it IS the base. Reading it instead of a caller-supplied pointer
// removes the ambiguity rather than relying on the caller to resolve it.
void overlayCanvasGray() {
  if (!g_canvas || !g_lsb || !g_msb) return;
  auto* dst = static_cast<uint8_t*>(g_canvas->getBuffer());
  if (!dst) return;
  for (uint16_t y = 0; y < g_h; ++y) {
    const uint8_t* lrow = g_lsb + static_cast<uint32_t>(y) * g_wb;
    const uint8_t* mrow = g_msb + static_cast<uint32_t>(y) * g_wb;
    uint8_t* drow = dst + static_cast<uint32_t>(y) * g_w;
    for (uint16_t bx = 0; bx < g_wb; ++bx) {
      const uint8_t l = lrow[bx], m = mrow[bx];
      if ((l | m) == 0) continue;  // no selector bits in this byte — leave the B/W run alone
      for (uint8_t bit = 0; bit < 8; ++bit) {
        const uint8_t mask = 0x80 >> bit;
        const bool lb = (l & mask) != 0, mb = (m & mask) != 0;
        if (!lb && !mb) continue;
        drow[bx * 8 + bit] = (mb && !lb) ? g_grayLight : g_grayDark;
      }
    }
  }
}

// --- panel optical response ------------------------------------------------
//
// The panel's 16 canvas levels are NOT evenly spaced in reflectance, and on this
// waveform they are not even 16 distinct levels. Simulating the clean bank's
// columns for the LilyGo's mid temperature range gives, per canvas level:
//
//   level:    0  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15
//   optical:  0  3  5  5  5  6  6  7  8  9  9  9 12 13 15 17
//
// Eleven distinct positions out of a 17-frame span, bunched hard in the middle:
// levels 2-4 are one shade, 9-11 another. Handing such a panel a LINEAR ramp
// spends most of the input range inside that cluster, and the result reads as
// about six bands -- which is what a 16-level sleep image first looked like on
// the hardware, near enough to the 4-level one to be worth nothing.
//
// So the ramp is linearised against the panel instead of against the encoding:
// equal steps of input luminance become equal steps of OPTICAL position, with
// Panel_EPD's Bayer cell dithering between the levels that bracket each target.
//
// Derived from the LUT at begin() rather than tabulated, so regenerating the
// waveform (or picking a different temperature range, which changes both the
// spacing AND the count) carries the correction along with it.

// Display-response brightening, applied before the optical linearisation below.
//
// Linearising against the waveform makes output REFLECTANCE track input
// luminance, which is correct in the physical sense and too dark in the visual
// one: this panel's white is a fraction of paper's, so a mid-grey rendered at
// exactly half reflectance reads much darker than the same pixel on a monitor.
// The dual-plane path has always compensated for this -- the whole difference
// between quantizeGray4's DisplayTuned and Native modes is thresholds of
// 30/50/140 against the even 43/128/213, described there as "a deliberate,
// output-referred brightening".
//
// Deliberately NOT reverse-engineered from those thresholds. They decide which
// of four levels a pixel lands on, and reading them as a transfer function
// implies a shadow segment mapping 30..50 onto 43..128 -- an enormous contrast
// boost across 20 input values that would posterise everything it touched. Four
// thresholds simply do not determine a curve. A gamma does the same job smoothly
// and has one number to argue about.
//
// That number wants a human looking at the panel, so it is a build flag. Raise
// it to brighten.
#ifndef FREEINK_GRAY8_GAMMA
#define FREEINK_GRAY8_GAMMA 2.2f
#endif

uint8_t g_grayRemap[256];    // linear luminance -> canvas byte
uint8_t g_grayOptical[16];   // canvas level -> final optical position
uint8_t g_grayDrive = 0;     // rail-to-rail drive length, in frames
uint8_t g_grayDistinct = 4;  // optical positions this bank actually resolves
bool g_grayRemapReady = false;

// Walk one destination column and report where a pixel ends up, in frames above
// the black rail. Drive codes are the vendor's: 1 = toward black, 2 = toward
// white, 0 = undriven.
int simulateGrayColumn(const uint32_t* lut, size_t steps, int level, int start, int rail) {
  int pos = start;
  const int shift = level * 2;
  for (size_t phase = 0; phase < steps; ++phase) {
    const uint8_t code = (lut[phase] >> shift) & 3;
    if (code == 1) {
      --pos;
    } else if (code == 2) {
      ++pos;
    }
    if (pos < 0) pos = 0;
    if (pos > rail) pos = rail;
  }
  return pos;
}

void buildGrayResponse(const uint32_t* lut, size_t steps) {
  g_grayRemapReady = false;
  g_grayDistinct = 4;
  if (!lut || steps == 0) return;

  // The rail-to-rail drive length is the largest clamp at which EVERY column
  // still lands on ONE position from every starting state. That source
  // independence is exactly the property the vendor bank is cut for (see
  // tools/gen_ed047tc2_waveform.py), so searching for it derives the number and
  // checks the table is the shape this code assumes, in one pass. Scanning
  // upward and stopping at the first failure costs ~drive iterations, not 64:
  // the property holds for every rail below the true one.
  int drive = 0;
  for (int rail = 1; rail <= 63; ++rail) {
    bool independent = true;
    for (int level = 0; level < 16 && independent; ++level) {
      const int first = simulateGrayColumn(lut, steps, level, 0, rail);
      for (int start = 1; start <= rail; ++start) {
        if (simulateGrayColumn(lut, steps, level, start, rail) != first) {
          independent = false;
          break;
        }
      }
    }
    if (!independent) break;
    drive = rail;
  }
  if (drive < 2) {
    Serial.printf("[epd] gray response: no source-independent rail found; native grayscale disabled\n");
    return;
  }

  bool seen[64] = {false};
  uint8_t distinct = 0;
  for (int level = 0; level < 16; ++level) {
    g_grayOptical[level] = static_cast<uint8_t>(simulateGrayColumn(lut, steps, level, 0, drive));
    if (!seen[g_grayOptical[level]]) {
      seen[g_grayOptical[level]] = true;
      ++distinct;
    }
  }
  // Monotonicity is what makes the inversion below well defined. A bank that
  // fails it is not one this correction can describe, so leave the remap off and
  // let the host fall back to the dual-plane path rather than ship a ramp that
  // goes backwards somewhere in the middle.
  for (int level = 1; level < 16; ++level) {
    if (g_grayOptical[level] < g_grayOptical[level - 1]) {
      Serial.printf("[epd] gray response: non-monotonic at level %d; native grayscale disabled\n", level);
      return;
    }
  }

  g_grayDrive = static_cast<uint8_t>(drive);
  g_grayDistinct = distinct;

  for (int v = 0; v < 256; ++v) {
    const float brightened = powf(static_cast<float>(v) / 255.0f, 1.0f / (FREEINK_GRAY8_GAMMA));
    const float target = brightened * drive;
    if (target >= g_grayOptical[15]) {
      g_grayRemap[v] = 255;
      continue;
    }
    // The LAST level of the plateau at or below the target, so the dither pairs
    // it with a level of a DIFFERENT shade. Pairing within a plateau would
    // spread a pixel over two encodings of the same optical position and buy
    // nothing.
    int lower = 0;
    for (int level = 0; level < 15; ++level) {
      if (g_grayOptical[level] <= target && g_grayOptical[level + 1] > target) lower = level;
    }
    const float lo = g_grayOptical[lower];
    const float hi = g_grayOptical[lower + 1];
    const float frac = (hi > lo) ? (target - lo) / (hi - lo) : 0.0f;
    // Panel_EPD reads a canvas byte c as level ((c + bayer - 8) >> 4), so
    // c = level * 16 + 8 lands exactly and the values between dither across the
    // pair in sixteenths.
    const int byte = static_cast<int>((static_cast<float>(lower) + frac) * 16.0f + 8.5f);
    g_grayRemap[v] = static_cast<uint8_t>(byte < 0 ? 0 : (byte > 255 ? 255 : byte));
  }
  g_grayRemapReady = true;

  Serial.printf("[epd] gray response: %u frames of drive, %u distinct levels, gamma %.2f, optical", drive, distinct,
                static_cast<double>(FREEINK_GRAY8_GAMMA));
  for (int level = 0; level < 16; ++level) Serial.printf(" %u", g_grayOptical[level]);
  Serial.printf("\n");
}

// The epd_mode of the last base push. Panel_EPD's per-pixel diff keys on the
// epd_mode LUT offset, so the grayscale overlay must be pushed with the SAME mode
// the base used or every pixel is re-driven (a full-screen flash). displayGray()
// used to hardcode epd_fast while display() maps HALF/FULL to epd_text, so any
// page refreshed with those modes flashed when its AA pass ran.
lgfx::epd_mode::epd_mode_t g_lastBaseEpdMode = lgfx::epd_mode::epd_fast;

// Wait out a refresh this driver just queued.
//
// waitDisplay() alone can return before the refresh has begun: Panel_EPD's
// display() raises _display_busy, yields (vTaskDelay(1)), and only then posts
// the job. The yield lets the panel task reach the top of its loop, where it
// assigns _display_busy = remain unconditionally -- false on an idle panel --
// clearing the flag the caller just raised, then blocking on a queue the job
// has not reached. Between xQueueSend() returning and the task waking, the flag
// reads false for a refresh that has not started, so a caller that trusts it
// walks straight into the panel task's diff copy and tears it. Torn step state
// is how a pixel ends up with a step index that never terminates, `remain`
// never clears, and the next waitDisplay() blocks forever -- the reader frozen
// with input still alive.
//
// Yielding first lets the panel task ingest the job and re-raise the flag; the
// wait after it then means what it says. (1.5.16 shipped this, 1.5.17 reverted
// it on a ghosting suspicion; the ghosting survived the revert, which clears
// this guard of that charge.)
void settleDisplay() {
  vTaskDelay(pdMS_TO_TICKS(2));
  g_dev.waitDisplay();
}

void pushCanvas(lgfx::epd_mode::epd_mode_t epdMode) {
  if (!g_canvas) return;
  g_dev.waitDisplay();
#if defined(LGFX_EPD_PUSH_TRACE) && LGFX_EPD_PUSH_TRACE
  const uint32_t tStart = millis();
#endif
  g_dev.setEpdMode(epdMode);
  g_canvas->pushSprite(0, 0);  // commits to the panel; Panel_EPD runs the refresh
  settleDisplay();
#if defined(LGFX_EPD_PUSH_TRACE) && LGFX_EPD_PUSH_TRACE
  // The PLAIN push, traced alongside the graded one so the log shows EVERY
  // refresh this panel is asked for. A second push nobody accounts for is
  // indistinguishable from a slow waveform when only one of the two is traced.
  Serial.printf("[epd] plain push: mode=%d took=%lums\n", (int)epdMode, (unsigned long)(millis() - tStart));
#endif
}

// Push the canvas keeping its grey levels, then refresh through the differential
// bank.
//
// Panel_EPD reads the epd_mode twice, at two different moments, and they do not
// have to agree. _draw_pixels() reads it while the sprite is being copied into
// the panel's 4bpp buffer, and in epd_fast/epd_fastest it Bayer-dithers every
// pixel to one of the two rails there and then -- that is what turned the AA
// greys into hard black speckle along glyph edges. task_update() reads it again
// when the refresh is queued, and only the fast modes skip lut_eraser, the
// preliminary pass that drives everything toward mid grey and shows as a flash.
//
// Splitting auto-display lets each read see the mode it should: quality while the
// pixels land (16 levels, no dither), fast when the refresh goes out (no eraser,
// and the same LUT bank the B/W base used, so Panel_EPD's per-pixel diff still
// skips everything that did not change).
void pushCanvasGraded(lgfx::epd_mode::epd_mode_t refreshMode) {
  if (!g_canvas) return;
  g_dev.waitDisplay();
#if defined(LGFX_EPD_PUSH_TRACE) && LGFX_EPD_PUSH_TRACE
  const uint32_t tWait = millis();
#endif
  g_dev.setEpdMode(lgfx::epd_mode::epd_quality);
  g_dev.setAutoDisplay(false);
  g_canvas->pushSprite(0, 0);  // writes the panel buffer, queues no refresh
  g_dev.setAutoDisplay(true);
  g_dev.setEpdMode(refreshMode);
#if defined(LGFX_EPD_PUSH_TRACE) && LGFX_EPD_PUSH_TRACE
  const uint32_t tSprite = millis();
#endif
  g_dev.display();  // covers the rect pushSprite accumulated
#if defined(LGFX_EPD_PUSH_TRACE) && LGFX_EPD_PUSH_TRACE
  const uint32_t tDisplay = millis();
#endif
  settleDisplay();
#if defined(LGFX_EPD_PUSH_TRACE) && LGFX_EPD_PUSH_TRACE
  // Splits the graded push into its three parts so a slow one can be attributed.
  // The mode is NAMED rather than numbered because the numbering invites exactly
  // the wrong reading: LovyanGFX's enum starts at 1 (quality=1, text=2, fast=3,
  // fastest=4), so the fast bank is 3 and a "2" is the CLEAN bank's eraser pass —
  // the flash — not a faster one.
  //
  // display= is near zero by design: Panel_EPD queues the refresh and returns.
  // The waveform is settle=, so that is the number to read.
  const char* modeName = refreshMode == lgfx::epd_mode::epd_quality   ? "quality"
                         : refreshMode == lgfx::epd_mode::epd_text    ? "text(clean bank, eraser)"
                         : refreshMode == lgfx::epd_mode::epd_fast    ? "fast(diff bank)"
                         : refreshMode == lgfx::epd_mode::epd_fastest ? "fastest"
                                                                      : "?";
  Serial.printf("[epd] graded push: mode=%s sprite=%lums display=%lums settle=%lums\n", modeName,
                (unsigned long)(tSprite - tWait), (unsigned long)(tDisplay - tSprite),
                (unsigned long)(millis() - tDisplay));
#endif
}

}  // namespace
#endif  // FREEINK_DRIVER_LGFX_EPD

LgfxEpdDriver::LgfxEpdDriver(const LgfxEpdConfig& cfg) : _cfg(cfg) {}

PanelGeometry LgfxEpdDriver::geometry() const {
  const uint16_t w = BoardConfig::ACTIVE.displayWidth;
  const uint16_t h = BoardConfig::ACTIVE.displayHeight;
  const uint16_t wb = w / 8;
  return {w, h, wb, static_cast<uint32_t>(wb) * h};
}

void LgfxEpdDriver::begin(EpdBus& bus) {
  (void)bus;
#if FREEINK_DRIVER_LGFX_EPD
  g_hooks = &_cfg.power;
  g_dev.setup(_cfg, BoardConfig::ACTIVE.displayWidth, BoardConfig::ACTIVE.displayHeight);
  g_dev.init();
  g_dev.setRotation(_cfg.rotation);
  g_dev.setEpdMode(lgfx::epd_mode::epd_fast);
  g_grayDark = _cfg.grayDark;
  g_grayLight = _cfg.grayLight;
  allocCanvas(BoardConfig::ACTIVE.displayWidth, BoardConfig::ACTIVE.displayHeight);
  // Against lutText: displayGray8Canvas() forces the clean bank, so that is the
  // waveform whose response the correction has to invert.
  buildGrayResponse(_cfg.lutText, _cfg.lutTextStep);
#endif
}

void LgfxEpdDriver::display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) {
  (void)bus;
  (void)prev;
#if FREEINK_DRIVER_LGFX_EPD
  fillCanvasBW(fb);  // expand the 1-bpp frame into the gray canvas
  g_lastBaseEpdMode = epdModeFor(mode);
  pushCanvas(g_lastBaseEpdMode);
  if (turnOff) g_dev.sleep();
#else
  (void)fb;
  (void)mode;
  (void)turnOff;
#endif
}

// One render, one push: the whole page -- text and its anti-aliasing greys --
// reaches the panel as a single waveform.
//
// The two-push flow this replaces (B/W base, then a grey overlay push) existed
// to normalize fringe pixels to black before a from-black grey nudge, because a
// destination-indexed LUT cannot see where a pixel came from. The fast bank's
// grey columns are now self-normalizing (saturate at the white rail, then walk
// down to the level), so the base pass has nothing left to do and the page has
// no intermediate state to show: it arrives finished, or it has not arrived.
//
// The charge story rides on the same property. Under the old flow every fringe
// pixel swung black-to-grey through two pushes on every page turn, with a net
// drive imbalance each time; under one push, Panel_EPD's diff drives a pixel
// only when its target changes, and every grey drive begins with a saturating
// rail visit that erases accumulated bias.
void LgfxEpdDriver::displayGrayFrame(EpdBus& bus, const uint8_t* fb, RefreshMode mode, bool turnOff) {
  (void)bus;
#if FREEINK_DRIVER_LGFX_EPD
  if (!fb) return;
  g_dev.waitDisplay();  // never write the canvas while a refresh may be in flight
  fillCanvasBW(fb);
  overlayCanvasGray();
  // HALF/FULL map to the GC16-style clean bank, whose columns land every level
  // exactly, so the periodic scrub page carries its greys too. FAST takes the
  // differential bank. Either way the write itself must be graded -- a fast-mode
  // write Bayer-dithers the greys to the rails before any LUT is consulted.
  g_lastBaseEpdMode = epdModeFor(mode);
  pushCanvasGraded(g_lastBaseEpdMode);
  if (turnOff) g_dev.sleep();
#else
  (void)fb;
  (void)mode;
  (void)turnOff;
#endif
}

void LgfxEpdDriver::copyGrayscaleLsb(EpdBus& bus, const uint8_t* lsb) {
  (void)bus;
#if FREEINK_DRIVER_LGFX_EPD
  if (g_lsb && lsb) memcpy(g_lsb, lsb, static_cast<size_t>(g_wb) * g_h);
#else
  (void)lsb;
#endif
}

void LgfxEpdDriver::copyGrayscaleMsb(EpdBus& bus, const uint8_t* msb) {
  (void)bus;
#if FREEINK_DRIVER_LGFX_EPD
  if (g_msb && msb) memcpy(g_msb, msb, static_cast<size_t>(g_wb) * g_h);
#else
  (void)msb;
#endif
}

void LgfxEpdDriver::writeGrayscalePlaneStrip(EpdBus& bus, GrayPlane plane, const uint8_t* rows, uint16_t yStart,
                                             uint16_t numRows) {
  (void)bus;
#if FREEINK_DRIVER_LGFX_EPD
  uint8_t* dstPlane = (plane == GrayPlane::Lsb) ? g_lsb : g_msb;
  if (!dstPlane || !rows) return;
  const uint32_t offset = static_cast<uint32_t>(yStart) * g_wb;
  memcpy(dstPlane + offset, rows, static_cast<size_t>(numRows) * g_wb);
#else
  (void)plane;
  (void)rows;
  (void)yStart;
  (void)numRows;
#endif
}

void LgfxEpdDriver::displayGray(EpdBus& bus, const uint8_t* fb, bool turnOff, const unsigned char* lut,
                                bool factoryMode) {
  (void)bus;
  (void)lut;
  (void)factoryMode;
#if FREEINK_DRIVER_LGFX_EPD
  (void)fb;             // the canvas from the base push IS the base; see overlayCanvasGray()
  overlayCanvasGray();  // darken only the pixels the planes select
  // Refresh under the mode the base push used. Panel_EPD's per-pixel diff keys
  // on the epd_mode LUT offset, so switching modes here re-drives every pixel --
  // a full-screen flash on any page the host refreshed with HALF or FULL.
  //
  // The pixel write is a separate question from the refresh, and on a board
  // whose fast bank carries grey columns it must not go out under a fast mode:
  // _draw_pixels() Bayer-dithers to the two rails there, which is what turned
  // the greys into black speckle. pushCanvasGraded() writes under a graded mode
  // and refreshes under this one.
  if (_cfg.grayNudgeInFastBank) {
    pushCanvasGraded(g_lastBaseEpdMode);
  } else {
    pushCanvas(g_lastBaseEpdMode);
  }
  if (turnOff) g_dev.sleep();
#else
  (void)fb;
  (void)turnOff;
#endif
}

void LgfxEpdDriver::cleanupGrayscaleBuffers(EpdBus& bus, const uint8_t* bw) {
  (void)bus;
#if FREEINK_DRIVER_LGFX_EPD
  if (!bw) return;
  fillCanvasBW(bw);
#else
  (void)bw;
#endif
}

uint8_t* LgfxEpdDriver::borrowGray8Canvas(uint16_t* stride) {
#if FREEINK_DRIVER_LGFX_EPD
  if (!g_canvas) return nullptr;
  auto* buf = static_cast<uint8_t*>(g_canvas->getBuffer());
  if (!buf) return nullptr;
  // Never hand out a buffer the panel task may still be scanning.
  g_dev.waitDisplay();
  if (stride) *stride = g_w;
  return buf;
#else
  (void)stride;
  return nullptr;
#endif
}

// Display a host-painted 8-bit canvas at the panel's full 16 levels.
//
// No plane encoding and no B/W base: the canvas IS the frame. pushCanvasGraded()
// writes it under epd_quality, which is the branch of Panel_EPD::_draw_pixels()
// that keeps sixteen levels -- `min(15, max(0, (v + bayer - 8) >> 4))` -- rather
// than the fast branch that thresholds every pixel to a rail.
//
// The refresh mode is forced to the clean bank rather than taken from the caller.
// The fast bank carries columns for the two AA greys and nothing else, so a
// mid-level asked of it lands on a column the LUT leaves undriven and the pixel
// stays black. PanelDriver::displayGray8Canvas documents the substitution.
uint8_t LgfxEpdDriver::grayLevels() const {
#if FREEINK_DRIVER_LGFX_EPD
  // What the bank measurably resolves, not what the panel is sold as. The
  // datasheet's 16 is the encoding; buildGrayResponse() counts the distinct
  // optical positions the vendor waveform actually lands (11 at room
  // temperature, 9 at the warm end), and reports the dual-plane floor of 4 if
  // the response could not be derived -- which routes callers back to the
  // plane path rather than promising depth this driver cannot deliver.
  return g_grayDistinct;
#else
  return 4;
#endif
}

void LgfxEpdDriver::displayGray8Canvas(EpdBus& bus, RefreshMode mode, bool turnOff) {
  (void)bus;
  (void)mode;
#if FREEINK_DRIVER_LGFX_EPD
  if (!g_canvas) return;
  // Linearise against the panel. The host paints in even steps of luminance;
  // this turns them into even steps of REFLECTANCE, which on this waveform is a
  // very different ramp (see buildGrayResponse). One pass over the canvas with a
  // byte LUT, on the frame that is about to be thrown at a 51-phase refresh.
  if (g_grayRemapReady) {
    if (auto* buf = static_cast<uint8_t*>(g_canvas->getBuffer())) {
      const size_t pixels = static_cast<size_t>(g_w) * g_h;
      for (size_t i = 0; i < pixels; ++i) buf[i] = g_grayRemap[buf[i]];
    }
  }
  g_lastBaseEpdMode = lgfx::epd_mode::epd_text;
  pushCanvasGraded(g_lastBaseEpdMode);
  if (turnOff) g_dev.sleep();
#else
  (void)turnOff;
#endif
}

void LgfxEpdDriver::deepSleep(EpdBus& bus) {
  (void)bus;
#if FREEINK_DRIVER_LGFX_EPD
  // Settle first. The panel task re-asserts the rails for the duration of its
  // diff pass, so powering down while a refresh is in flight lets it power them
  // straight back up -- and settleDisplay(), not a bare waitDisplay(), is what
  // actually waits here (see its comment: the flag reads clear for a refresh
  // that has not started yet).
  settleDisplay();
  g_dev.sleep();

  // Then power down unconditionally, past the bus's cached state. sleep() above
  // routes through powerControl(false), which short-circuits whenever the rails
  // are already believed down -- the normal case, since the last refresh turned
  // them off -- so on its own the whole power-down rests on that earlier
  // transition having worked. The hook is idempotent and costs a few I2C
  // writes; deep sleep is exactly where being wrong is most expensive.
  // Adopted from jetaudio's crosspoint-aurora.
  if (g_hooks && g_hooks->powerOff) g_hooks->powerOff();
#endif
}

// Per-board config injection. This driver has NO universal default — the bus pins
// and power hooks are entirely board-specific — so a LilyGo-class board defines
// `const LgfxEpdConfig& yourConfig();` in namespace freeink and builds with
// -DFREEINK_LGFX_EPD_CONFIG=yourConfig. The SDK's board-support libraries provide
// the default configs for FREEINK_DEVICE_LILYGO (BoardT5S3) and
// FREEINK_DEVICE_PAPERS3 (BoardPaperS3) builds.
#if FREEINK_DEVICE_LILYGO
const LgfxEpdConfig& lilygoT5S3LgfxConfig();
PanelDriver& lgfxEpdDriver() {
  static LgfxEpdDriver instance(lilygoT5S3LgfxConfig());
  return instance;
}
#elif FREEINK_DEVICE_PAPERS3 && !defined(FREEINK_LGFX_EPD_CONFIG)
const LgfxEpdConfig& m5PaperS3LgfxConfig();
PanelDriver& lgfxEpdDriver() {
  static LgfxEpdDriver instance(m5PaperS3LgfxConfig());
  return instance;
}
#elif defined(FREEINK_LGFX_EPD_CONFIG)
const LgfxEpdConfig& FREEINK_LGFX_EPD_CONFIG();
PanelDriver& lgfxEpdDriver() {
  static LgfxEpdDriver instance(FREEINK_LGFX_EPD_CONFIG());
  return instance;
}
#elif FREEINK_DRIVER_LGFX_EPD
#error "FREEINK_DRIVER_LGFX_EPD requires a board config: define `const LgfxEpdConfig& yourConfig();` in namespace freeink and build with -DFREEINK_LGFX_EPD_CONFIG=yourConfig"
#else
// Driver not selected in this build: provide a stub so the accessor still links if
// referenced. Never called (the facade only selects it under FREEINK_DRIVER_LGFX_EPD).
PanelDriver& lgfxEpdDriver() {
  static const LgfxEpdConfig kNone = {};
  static LgfxEpdDriver instance(kNone);
  return instance;
}
#endif

}  // namespace freeink
