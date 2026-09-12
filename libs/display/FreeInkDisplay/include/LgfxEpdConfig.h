#pragma once

#include <Arduino.h>

namespace freeink {

// Board-supplied power glue, called from the LovyanGFX bus lifecycle. The board
// implements these (for example PCA9535 expander + TPS65185 PMIC on LilyGo T5 S3)
// and injects them in its LgfxEpdConfig. Any hook may be null.
struct LgfxEpdPowerHooks {
  bool (*prepare)();
  bool (*powerOn)();
  // Returns false if the rails could not be proven down (e.g. an I2C write to a
  // power expander failed). The caller leaves the cached rail state UNKNOWN on
  // false so the next transition re-issues the hook instead of trusting a stale
  // belief; see FreeInkBusEPD::powerControl.
  bool (*powerOff)();
};

// LovyanGFX parallel-EPD wiring. Geometry is not here; it comes from the active
// BoardProfile, like all drivers. This carries only bus/panel specifics.
struct LgfxEpdConfig {
  int8_t dataPins[8];
  int8_t pinSph;
  int8_t pinSpv;
  int8_t pinOe;
  int8_t pinLe;
  int8_t pinCl;
  int8_t pinCkv;
  int8_t pinPwr;
  uint32_t busHz;
  uint8_t linePadding;
  uint8_t rotation;
  LgfxEpdPowerHooks power;
  const uint32_t* lutQuality = nullptr;
  size_t lutQualityStep = 0;
  const uint32_t* lutText = nullptr;
  size_t lutTextStep = 0;
  const uint32_t* lutFast = nullptr;
  size_t lutFastStep = 0;
  const uint32_t* lutFastest = nullptr;
  size_t lutFastestStep = 0;

  // The 8-bit canvas values the driver writes for the two anti-aliasing greys.
  // Panel_EPD quantises a canvas byte to a 4-bit level as (v + bayer - 8) >> 4,
  // so only v == (level << 4) | 8 lands on one level for every cell of the Bayer
  // matrix; anything else alternates between two levels and shows up as speckle
  // along glyph edges. grayLevelByte() builds a safe value from a level.
  //
  // Which level to ask for is a property of the board's waveform: the LUT drives
  // grey destinations by column, so a canvas level the LUT has no column for
  // simply stays at whatever the B/W base left it on.
  //
  // The defaults are the even thirds this driver has always written. They are not
  // Bayer-exact, which does not matter on a board that leaves
  // grayNudgeInFastBank false: that path dithers the canvas anyway, and these are
  // the densities it has been dithering since the driver was written.
  uint8_t grayDark = 0x55;
  uint8_t grayLight = 0xAA;

  // True when the board's epd_fast LUT carries grey columns as well as the two
  // B/W rails, so the grayscale push can go out through the differential bank:
  // no lut_eraser flash, and the same bank the B/W base used, which is what lets
  // Panel_EPD's per-pixel diff keep skipping everything that did not change.
  //
  // Left false for a board on LovyanGFX's stock LUTs, whose fast bank drives only
  // the rails. Those boards keep the plain epd_fast push they have always had --
  // the greys still come out dithered there, which is a waveform gap on that
  // board, not something this driver can paper over.
  bool grayNudgeInFastBank = false;

  // True when a Half refresh should go out through the DIFFERENTIAL bank rather
  // than the clean one, leaving Full as the board's only clean refresh.
  //
  // Set this on a board whose epd_fast LUT saturates every drive it makes -- each
  // column carrying a full rail-to-rail impulse, so the destination lands from any
  // source and the pixel's history is erased by the clamp. Such a bank does not
  // accumulate ghosting, which is what makes it safe to spend on the periodic and
  // post-transition refreshes that Half covers.
  //
  // The gain is not speed, it is the artefact the clean bank produces here: see
  // epdModeFor() in LgfxEpdDriver.cpp for why two consecutive clean refreshes on
  // such a panel show both pages at once and leave the old one as an imprint.
  //
  // Left false for a board on LovyanGFX's stock LUTs, whose fast bank drives the
  // rails for a fixed few frames and does need the clean bank behind it.
  bool halfUsesFastBank = false;
};

// Canvas byte that quantises to exactly `level` for every Bayer cell.
constexpr uint8_t grayLevelByte(uint8_t level) { return static_cast<uint8_t>((level << 4) | 8); }

// --- LUT block budget ------------------------------------------------------
//
// Panel_EPD packs a pixel's refresh progress into a uint16_t as
// (lut_block << 8) | level, and blit_dmabuf reads it back through a SIGNED
// cast and skips the pixel when the result is negative -- bit 15 means "this
// pixel is idle". So no block index may ever reach 128: the five banks
// together have to fit in 128 blocks.
//
// Overrunning it fails SILENTLY and globally. Every pixel whose waveform
// reaches block 128 reads as idle mid-refresh, so refreshes stop completing --
// which is how a cool-temperature waveform once blanked this panel below ~27 C
// while warmer boots worked, and it cost a debugging session because nothing
// reports it. Hence lgfxEpdLutBlocks() and the check in LgfxEpdDriver::begin().
inline constexpr size_t LGFX_EPD_LUT_BLOCKS_MAX = 128;

// Panel_EPD's hardcoded eraser bank, prepended to every epd_text / epd_quality
// refresh and not supplied by config. Two drive rows, a park row and the
// terminator (Panel_EPD.cpp, lut_eraser / lut_eraser_step).
inline constexpr size_t LGFX_EPD_LUT_BLOCKS_ERASER = 4;

// What Panel_EPD substitutes into a slot this config leaves empty. Counted from
// its stock tables; a board on all four pays 85 of the 128 blocks.
inline constexpr size_t LGFX_EPD_LUT_BLOCKS_STOCK_QUALITY = 32;
inline constexpr size_t LGFX_EPD_LUT_BLOCKS_STOCK_TEXT = 32;
inline constexpr size_t LGFX_EPD_LUT_BLOCKS_STOCK_FAST = 10;
inline constexpr size_t LGFX_EPD_LUT_BLOCKS_STOCK_FASTEST = 7;

// Blocks this config will actually occupy, stock substitution included. Mirrors
// the accumulation in Panel_EPD::init_intenal().
constexpr size_t lgfxEpdLutBlocks(const LgfxEpdConfig& c) {
  return LGFX_EPD_LUT_BLOCKS_ERASER +
         ((c.lutQuality && c.lutQualityStep) ? c.lutQualityStep : LGFX_EPD_LUT_BLOCKS_STOCK_QUALITY) +
         ((c.lutText && c.lutTextStep) ? c.lutTextStep : LGFX_EPD_LUT_BLOCKS_STOCK_TEXT) +
         ((c.lutFast && c.lutFastStep) ? c.lutFastStep : LGFX_EPD_LUT_BLOCKS_STOCK_FAST) +
         ((c.lutFastest && c.lutFastestStep) ? c.lutFastestStep : LGFX_EPD_LUT_BLOCKS_STOCK_FASTEST);
}

}  // namespace freeink
