#pragma once

// Onyx Picco SE0400NQW47 panel — the controller the stock firmware calls
// "SSD2677" (identified by CMD 0x70 -> 07 01 01). Its command set is
// UltraChip-style, not SSD1677: panel setting 0x00, window 0x83, data 0x10, one
// 535-byte LUT via 0x20, power on/off 0x04/0x02, refresh 0x12, deep sleep 0x07.
// BUSY is active-LOW.
//
// Each frame goes out as 2 bits per pixel through a single 0x10 burst: the
// previous frame in the high bit, the new frame in the low bit; the LUT picks
// the transition. Init list, LUTs and bit packing are from the stock firmware
// (build 2912). Grayscale is the stock overlay AA mode (slot-5 LUT): the LSB
// and MSB masks are packed as the low/high bit of each pixel, with dark pixels
// (both masks) sent as LSB-only. Images use the panel's dedicated
// slot-4 waveform with the two complete image planes. Reader pages on both
// boards take one combined pass with lut_picco_se0400_text instead of base +
// overlay.
//
// The newer Seeed Sticky carries the same controller with OTP waveforms: its
// stock firmware (v1.2.0) uploads no LUT but latches an OTP waveform before each
// frame (0xE0 0x12, 0xE6 code, 0xA5), sends full refreshes with old == new, and
// keeps the rails on between fast refreshes. Its images use stock's OTP gray4
// waveform (one pass); text AA uses the Picco's uploaded overlay waveform.
//
// Selected by FreeInkDisplay when piccoProbePanel() reports PiccoSe0400nqw47, or
// on a Sticky when stickyHasSsd2677().

#include <BoardConfig.h>

#include "PanelDriver.h"
#include "X4ProTextAa.h"

namespace freeink {

class Ssd2677Driver : public PanelDriver {
 public:
  const char* controllerName() const override { return "SSD2677"; }
  uint32_t spiHz() const override;
  BusyPolarity busyPolarity() const override {
    return BoardConfig::ACTIVE.board == BoardConfig::Board::Sticky ? BusyPolarity::ActiveLowReady
                                                                   : BusyPolarity::ActiveLow;
  }
  PanelGeometry geometry() const override;

  // Also takes the text pass's planes, on the first reader page rather than at begin().
  void setReaderRefresh(bool reading) override;
  // Witch Reader's single push (PanelDriver::supportsGrayFrame): while setOnePassText() is on, a reader
  // page stages its planes, then displayGrayFrame() sends base and gray as the same rest pass that
  // displayGray() runs for a combined base. A clean refresh keeps two passes: the page in B/W, then
  // its gray as the two-pass overlay. The Picco only; the Sticky keeps the combined-base entry.
  bool supportsOnePassText() const override;
  void setOnePassText(bool on) override;
  bool supportsGrayFrame() const override { return _onePassText && supportsOnePassText() && textPassReady(); }
  void displayGrayFrame(EpdBus& bus, const uint8_t* fb, RefreshMode mode, bool turnOff) override;
  void setBackgroundHint(bool darkBackground) override { _darkBackground = darkBackground; }
  // BOOX 2959 runs the reader AA overlay after inverted pages too.
  bool grayscaleWhileInverted() const override { return BoardConfig::ACTIVE.board != BoardConfig::Board::Sticky; }
  void begin(EpdBus& bus) override;
  void deepSleep(EpdBus& bus) override;
  void powerOffBetweenFrames(EpdBus& bus) override;
  void display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) override;
  // The Picco's B/W frames and its single push defer: the start returns once the waveform is fired, with the
  // driver's history already saying what the glass will hold, and displayFinish() waits the waveform out, powers
  // off where the frame asked for it, and sends what a clean single push still owes (its gray). It reads no host
  // buffer. Every other entry that reaches the controller finishes a refresh in flight first. The two-pass gray
  // (copyGrayscale*, displayGray) and the Sticky's OTP frames block as before.
  bool supportsAsyncDisplay() const override;
  bool displayStart(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) override;
  void displayFinish(EpdBus& bus, const uint8_t* fb) override;
  bool displayGrayFrameStart(EpdBus& bus, const uint8_t* fb, RefreshMode mode, bool turnOff) override;

  // Overlay AA with the Picco's uploaded gray LUT on both boards. Sticky also
  // uses its OTP image waveform. Picco uploads its own slot-4 waveform.
  // Both take a complete image in one activation.
  GrayscaleCapabilities grayscaleCapabilities(GrayscaleMode mode = GrayscaleMode::Overlay) const override {
    // Reader pages: base and gray in one activation (lut_picco_se0400_text).
    if (mode == GrayscaleMode::Overlay && _readerRefresh && textPassReady())
      return {GrayscaleEncoding::OverlayMasks, GrayscaleBase::Combined, false, false, false};
    if (mode == GrayscaleMode::Overlay)
      return {GrayscaleEncoding::OverlayMasks, GrayscaleBase::Separate, false, false, false};
    if (mode == GrayscaleMode::Absolute)
      return {GrayscaleEncoding::AbsolutePlanes, GrayscaleBase::Combined, false, false, false};
    return {};
  }
  void requestResync(uint8_t) override {
    _textAa.invalidate();
    _textBegun = false;
    _absoluteGray = false;
    _grayPending = false;
    if (BoardConfig::ACTIVE.board != BoardConfig::Board::Sticky) _needsInitialFull = true;
  }
  void beginGrayscale(EpdBus& bus, const uint8_t* fb, GrayscaleMode mode, RefreshMode fallback, bool turnOff) override;
  void copyGrayscaleLsb(EpdBus& bus, const uint8_t* lsb) override;
  void copyGrayscaleMsb(EpdBus& bus, const uint8_t* msb) override;
  void displayGray(EpdBus& bus, const uint8_t* fb, bool turnOff, const unsigned char* lut, bool factoryMode) override;
  void cleanupGrayscaleBuffers(EpdBus& bus, const uint8_t* bw) override;
  // Lab (development builds): code = red * 2 + bw selects pixel table 1..4 of `lut`, a full 535-byte LUT.
  bool displayLabFrame(EpdBus& bus, const uint8_t* bw, const uint8_t* red, const uint8_t* lut) override;

  // Absolute 16 levels on the Picco SE0400NQW47 (not the Sticky: its OTP
  // waveforms were never calibrated for this). Three draw passes (dissolve,
  // coarse black, pull-back white) built at runtime from the doses; defaults
  // and the give-back tables in lut/Ssd2677Gray16Luts.h.
  bool supportsGray16Image() const override { return BoardConfig::ACTIVE.board != BoardConfig::Board::Sticky; }
  bool displayGray16Image(EpdBus& bus, const uint8_t* levels, RefreshMode mode, bool turnOff) override;
  bool setGray16Turn(uint8_t scrubEvery) override;
  // 16-level text: `levels` are the calibrated levels (as displayGray16Image);
  // the base must hold exactly the deep-path levels black.
  bool beginGray16Text(EpdBus& bus, const uint8_t* base, const uint8_t* soft = nullptr) override;
  bool finishGray16Text(EpdBus& bus, const uint8_t* levels, bool turnOff) override;
  bool setGray16TextSoftPull(uint8_t frames) override;
  // paths[l] = {black dose index 0..3, white dose index 0..3, deep 0/1}.
  bool setGray16Levels(EpdBus& bus, const uint8_t (*paths)[3]) override;
  bool setGray16Passes(EpdBus& bus, const uint8_t black[3], const uint8_t white[3], uint8_t erase) override;

 private:
  void initPanel(EpdBus& bus);
  void latchOtpWaveform(EpdBus& bus, uint8_t code);
  // exclusive: clear the high bit wherever the low bit is set.
  void sendPacked(EpdBus& bus, const uint8_t* hi, const uint8_t* lo, bool exclusive = false);
  bool ensureLsb();
  void powerOn(EpdBus& bus);
  void powerOff(EpdBus& bus);
  void powerAndRefresh(EpdBus& bus, bool turnOff);
  void settle(EpdBus& bus);  // wait out a deferred refresh (no-op when none)
  // Cross-dissolve codes from the old page's levels and the new base (1 bpp).
  void sendCrossCodes(EpdBus& bus, const uint8_t* oldLevels, const uint8_t* base, const uint8_t* soft);
  void refreshWith(EpdBus& bus, const unsigned char* lut, bool turnOff);
  // refreshWith() in two halves: the LUT, power on and the refresh command, then the wait and the power off.
  void startRefresh(EpdBus& bus, const unsigned char* lut, bool turnOff);
  void finishRefresh(EpdBus& bus);
  // finishRefresh(), then the gray a clean single push owes. What every entry runs before it touches the bus.
  void finishPending(EpdBus& bus);
  // Picco reader text: rest every pixel already at its target (see sendRestCodes).
  bool textPassReady() const;
  bool ensureRestText();
  void stageRestText(unsigned plane, const uint8_t* data);
  bool startRestText(EpdBus& bus);
  bool presentRestText(EpdBus& bus);
  bool presentOverlayText(EpdBus& bus);
  void sendRestCodes(EpdBus& bus, bool grayKnown);
  // Stream one 2-bit code per pixel: codes[level] for each 4-bit level, or
  // `solid` for every pixel when levels is null.
  void sendLevelCodes(EpdBus& bus, const uint8_t* levels, const uint8_t codes[16], uint8_t solid = 3);
  // Fast-turn dissolve codes from the image on the glass and the next one:
  // 11 nothing, 01 erase old ink, 10 new deep black, 00 both.
  void sendDissolveCodes(EpdBus& bus, const uint8_t* oldLevels, const uint8_t* newLevels);
  // A 16-level image on the glass gives its net drive back (toward white) and
  // arms a full refresh. Every frame path that replaces the image calls this.
  void exitGray16(EpdBus& bus);
  // Give the image on the glass its exact net drive back (toward white only:
  // no flash) and mark it gone; exitGray16 adds the full refresh.
  void balanceGray16(EpdBus& bus);

  // Per-level pixel codes for each pass, derived from the paths and doses.
  struct Gray16Codes {
    uint8_t deep[16], coarse[16], pull[16], balanceDeep[16], balanceCoarse[16], balanceFine[16];
    bool ink[16];  // the level drives anything (the dissolve erases it on the next fast turn)
    bool useDeep, useCoarse, useBalanceDeep;
  };
  static bool computeGray16Codes(const uint8_t (*paths)[3], const uint8_t black[3], const uint8_t white[3],
                                 Gray16Codes& out);
  void ensureGray16Config();  // defaults, codes and tables on first use
  void buildGray16Tables();   // the four draw tables from the current doses

  X4ProTextAa _textAa;  // Sticky reader text (full-target compose with a charge map)
  uint8_t* _textGray = nullptr;  // Picco: the page's staged gray (LSB | MSB); swaps with _lsb after a pass
  uint8_t* _textBase = nullptr;  // Picco: the page's B/W base, as the renderer reuses its buffer for the planes
  uint8_t* _textDark = nullptr;  // Picco single push: the page's staged LSB, apart for a clean refresh's overlay
  uint8_t _textStaged = 0;       // bit 0 LSB staged, bit 1 MSB, bit 2 the LSB kept in _textDark
  bool _restGrayOnGlass = false;  // the gray on the glass is the rest table's, so gray that stays may rest
  bool _textBegun = false;       // beginGrayscale() took a Picco reader page for the text pass
  bool _readerRefresh = false;
  bool _onePassText = false;  // the host sends reader pages through displayGrayFrame()
  bool _afterReader = false;  // last frame presented a book page
  bool _darkBackground = false;  // facade renders inverted frames
  bool _needsPowerOn = true;
  bool _needsInitialFull = true;
  // A waveform startRefresh() fired has not been waited out; the power off it owes; BUSY was seen to drop for it,
  // so BUSY back up says it is over.
  bool _refreshPending = false;
  bool _pendingPowerOff = false;
  bool _pendingSawBusy = false;
  // A clean single push's B/W page is in flight: what follows it, from the planes staged for the page.
  enum class GrayAfterPage : uint8_t { None, Overlay, RestPass };
  GrayAfterPage _grayAfterPage = GrayAfterPage::None;
  uint8_t _latchedWaveform = 0;  // Sticky: OTP waveform code currently latched (0 = none)
  bool _grayPending = false;
  bool _grayMaskReady = false;
  bool _absoluteGray = false;  // the current gray pass began as a complete image
  uint8_t* _lsb = nullptr;  // retained LSB mask: the renderer reuses its buffer for the MSB pass
  // Levels of the 16-level image on the glass (W*H/2 bytes, PSRAM), kept for
  // exitGray16's balance passes. Allocated on first use.
  uint8_t* _gray16 = nullptr;
  bool _gray16OnGlass = false;
  bool _textBaseUp = false;  // beginGray16Text ran; finishGray16Text owes the passes
  bool _textOnGlass = false;    // a 16-level text page is up; its levels are in _gray16
  bool _textDissolved = false;  // this text turn's base was the cross-dissolve
  uint8_t _textSoftLevel = 0xFF;  // deep level pulled back inside the dissolve (0xFF: none)
  uint8_t _textSoftPull = 0xFF;   // its frames there; 0xFF = its calibrated pull-back, 0 = off
  bool _holdPower = false;   // display() leaves the rails on (16-level text base)
  bool _deferRefresh = false;    // powerAndRefresh returns without waiting (text base)
  bool _refreshInFlight = false; // a deferred waveform may still run; settle() before any bus access
  unsigned long _refreshStartMs = 0;
  uint8_t _g16ScrubEvery = 0;  // fast turns between full clean turns; 0 = always full
  uint8_t _g16FastTurns = 0;   // fast turns since the last full one
  bool _g16Ready = false;
  uint8_t _g16Black[3] = {}, _g16White[3] = {}, _g16Erase = 0;
  uint8_t _g16Paths[16][3] = {};
  Gray16Codes _g16{};
  // Runtime draw tables (in the driver object: no heap, 4 x 535 bytes).
  uint8_t _lutDissolve[535] = {}, _lutDeep[535] = {}, _lutCoarse[535] = {}, _lutPull[535] = {};
  uint8_t _lutCross[535] = {};  // text page turn: erase + deep at once
  uint8_t* _old = nullptr;  // retained old plane (last displayed frame); host is single-buffer
                            // so `prev` is always null — mirror stock's obj+0x1020, seeded white
};

PanelDriver& ssd2677Driver();

// Net drive of pixel table 1..4 of a 535-byte LUT in frames, black positive.
int ssd2677LutDose(const unsigned char* lut, unsigned table);

}  // namespace freeink
