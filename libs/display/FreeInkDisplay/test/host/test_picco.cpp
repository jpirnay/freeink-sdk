// Onyx Picco: panel-ID probe picks the stock per-panel SSD1677 tables.
// Run once per panel (the driver is a process-wide singleton): yrd | depg | opm | unknown.
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include "esp_heap_caps.h"
#define private public
#include "FreeInkDisplay.h"
#include "src/driver/Ssd1677Driver.h"
#include "src/driver/Ssd2677Driver.h"
#include "src/driver/RestTextCodes.h"
#include "src/lut/Ssd1677Luts.h"
#include "src/lut/Ssd2677Gray16Luts.h"
#include "src/lut/Ssd2677Luts.h"
#undef private

using namespace freeink;
using Bytes = std::vector<uint8_t>;

static uint8_t lastRegister(const EpdBus& bus, uint8_t command) {
  for (auto i = bus.writes.rbegin(); i != bus.writes.rend(); ++i)
    if (i->command == command && !i->bytes.empty()) return i->bytes[0];
  assert(false);
  return 0;
}

static const Bytes* lastLut(const EpdBus& bus) {
  for (auto i = bus.writes.rbegin(); i != bus.writes.rend(); ++i)
    if (i->command == 0x32) return &i->bytes;
  return nullptr;
}

static void expectRefresh(Ssd1677Driver& d, RefreshMode mode, uint8_t ctrl2, const unsigned char* lut, bool turnOff = false) {
  EpdBus bus;
  const Bytes fb(48000, 0xAA);
  d.display(bus, fb.data(), fb.data(), mode, turnOff);
  assert(lastRegister(bus, 0x22) == ctrl2);
  if (d._cfg.piccoWaveforms) assert(d._isScreenOn == ((ctrl2 & 0x03) == 0));
  const Bytes* sent = lastLut(bus);
  if (lut) {
    assert(sent && *sent == Bytes(lut, lut + 105));
    assert(lastRegister(bus, 0x2C) == lut[109]);
  } else {
    assert(!sent);
  }
  if (d._cfg.fastLut == lut_picco_depg_custom_fast) {
    bool addressed = false;
    unsigned planes = 0;
    for (const auto& w : bus.writes) {
      if (w.command == 0x4F) addressed = true;
      if (w.command == 0x24 || w.command == 0x26) {
        assert(addressed);
        addressed = false;
        ++planes;
      }
    }
    assert(planes == 2);
  }
  assert(!d._customLutActive);
}

static void expectCustomDepg(Ssd1677Driver& d) {
  const auto* split = lut_picco_depg_custom_fast;
  assert(split[0] == 0x01 && split[10] == 0x88 && split[20] == 0x44);
  assert(split[50] == 8 && split[51] == 1 && split[52] == 9 && split[53] == 1);
  // Changing pixels retain 17 driven frames, with no drive during phase B.
  for (unsigned row : {10u, 20u}) {
    unsigned driven = 0;
    for (unsigned phase = 0; phase < 4; ++phase)
      if ((split[row] >> (6 - 2 * phase)) & 3) driven += split[50 + phase];
    assert(driven == 17 && ((split[row] >> 4) & 3) == 0);
  }
  for (unsigned i = 0; i < 105; ++i)
    if (i != 0 && i != 10 && i != 20 && (i < 50 || i > 53))
      assert(split[i] == lut_picco_depg_fast_slot1[i]);
  assert(!memcmp(lut_picco_depg_custom_fast + 105, lut_grayscale_sticky + 105, 5));
  EpdBus bus;
  Bytes prior(48000, 0x33), target(48000);
  for (size_t i = 0; i < target.size(); ++i) target[i] = static_cast<uint8_t>(i);
  for (bool reading : {false, true}) {
    for (bool dark : {false, true}) {
      d.begin(bus);
      d.setBackgroundHint(dark);
      d.setReaderRefresh(reading);
      d.display(bus, prior.data(), nullptr, RefreshMode::Full, false);
      bus.clear();
      d.display(bus, target.data(), nullptr, RefreshMode::Fast, false);
      const auto* lut = lut_picco_depg_custom_fast;
      assert(lastLut(bus) && *lastLut(bus) == Bytes(lut, lut + 105));
      assert(lastRegister(bus, 0x03) == lut[105]);
      assert(lastRegister(bus, 0x2C) == lut[109]);
      unsigned planes = 0, activations = 0;
      for (const auto& w : bus.writes) {
        if (w.command == 0x04) assert(w.bytes == Bytes(lut + 106, lut + 109));
        activations += w.command == 0x20;
        if (w.command != 0x24 && w.command != 0x26) continue;
        ++planes;
        Bytes expected = w.command == 0x24 ? target : prior;
        if (w.command == 0x26 && dark) {
          expected = target;
          for (auto& b : expected) b = static_cast<uint8_t>(~b);
        }
        assert(w.bytes == expected);
      }
      assert(planes == 2 && activations == 1);
      assert(Bytes(d._piccoOld, d._piccoOld + 48000) == target);
      bus.clear();
      d.display(bus, target.data(), nullptr, RefreshMode::Half, false);
      assert(lastLut(bus) && *lastLut(bus) == Bytes(lut_picco_depg_full, lut_picco_depg_full + 105));
      for (const auto& w : bus.writes)
        if (w.command == 0x24 || w.command == 0x26) assert(w.bytes == target);
    }
  }
  d.setBackgroundHint(false);
  d.setReaderRefresh(false);
}

static void expectRetainedGrayStrip(Ssd1677Driver& d) {
  EpdBus bus;
  Bytes bw(48000, 0xFF), lsb(48000, 0x55), msb(48000, 0x33);
  d.begin(bus);
  assert(!d.displayRetainedGrayscaleOverlay(bus, bw.data(), 3, 2, 11, 3));
  d.display(bus, bw.data(), nullptr, RefreshMode::Full, false);
  d.beginGrayscale(bus, bw.data(), GrayscaleMode::Absolute, RefreshMode::Half, false);
  d.copyGrayscaleLsb(bus, lsb.data());
  d.writeGrayscalePlaneStrip(bus, GrayPlane::Msb, msb.data(), 0, 480);
  d.displayGray(bus, msb.data(), true, nullptr, true);
  d.requestResync(1);  // facade finishes an absolute session this way
  for (int paint = 0; paint < 3; ++paint) {
    std::fill(bw.begin(), bw.end(), paint == 0 ? 0xF0 : 0x0F);
    const bool restore = paint == 2;
    bus.clear();
    assert(d.displayRetainedGrayscaleOverlay(bus, restore ? nullptr : bw.data(), 3, 2, 11, 3));
    unsigned activations = 0, planes = 0;
    for (const auto& w : bus.writes) {
      activations += w.command == 0x20;
      if (w.command != 0x24 && w.command != 0x26) continue;
      ++planes;
      assert(w.bytes.size() == 48000);
      const auto& original = w.command == 0x24 ? lsb : msb;
      for (size_t i = 0; i < original.size(); ++i) {
        uint8_t expected = original[i];
        if (!restore && i / 100 >= 2 && i / 100 < 5) {
          for (int bit = 0; bit < 8; ++bit) {
            const int x = (i % 100) * 8 + bit;
            if (x >= 3 && x < 14) {
              const uint8_t mask = 0x80 >> bit;
              expected = (expected & ~mask) | (bw[i] & mask);
            }
          }
        }
        assert(w.bytes[i] == static_cast<uint8_t>(~expected));
      }
    }
    // Factory gray powers down separately after its single image activation.
    assert(planes == 2 && activations == 2u);
  }
  bus.clear();
  assert(!d.displayRetainedGrayscaleOverlay(bus, bw.data(), 799, 0, 2, 1));
  assert(bus.writes.empty());
  d.display(bus, bw.data(), nullptr, RefreshMode::Fast, false);
  bus.clear();
  assert(!d.displayRetainedGrayscaleOverlay(bus, nullptr, 3, 2, 11, 3));
  assert(bus.writes.empty());
}

static void expectStockPiccoGray(Ssd1677Driver& d) {
  EpdBus bus;
  Bytes base(48000, 0x55), lsb(48000, 0x11), msb(48000, 0x33);
  d.begin(bus);
  d.display(bus, base.data(), nullptr, RefreshMode::Full, false);
  for (bool absolute : {false, true}) {
    d.beginGrayscale(bus, base.data(), absolute ? GrayscaleMode::Absolute : GrayscaleMode::Overlay,
                     RefreshMode::Fast, false);
    bus.clear();
    d.copyGrayscaleLsb(bus, lsb.data());
    d.writeGrayscalePlaneStrip(bus, GrayPlane::Msb, msb.data(), 0, 480);
    // Absolute planes use the X4 factory encoding (black=11), so they are complemented.
    for (const auto& w : bus.writes) {
      Bytes expected = w.command == 0x24 ? lsb : msb;
      if (absolute) for (auto& b : expected) b = static_cast<uint8_t>(~b);
      if (w.command == 0x24 || w.command == 0x26) assert(w.bytes == expected);
    }
    bus.clear();
    const bool wasOn = d._isScreenOn;
    d.displayGray(bus, msb.data(), absolute, nullptr, absolute);
    if (absolute) {
      // Images carry their own voltages: the X4 factory LUT, not a stock 105-byte
      // table that would inherit the last B/W LUT's VCOM/VSH2.
      assert(lastLut(bus) && *lastLut(bus) == Bytes(lut_factory_quality, lut_factory_quality + 105));
      assert(lastRegister(bus, 0x2C) == lut_factory_quality[109]);
      for (const auto& w : bus.writes)
        if (w.command == 0x04) assert(w.bytes == Bytes(lut_factory_quality + 106, lut_factory_quality + 109));
      // 0xCC paint, then a separate 0x03 shutdown.
      assert(lastRegister(bus, 0x22) == 0x03);
      assert(d._needsGrayClear && !d._isScreenOn);
      continue;
    }
    const auto* lut = d._cfg.grayLut;
    if (lut) {
      Bytes expected(lut, lut + 105);
      if (!absolute && d._cfg.fastLut == lut_picco_depg_custom_fast)
        for (unsigned index : {12u, 22u, 32u}) expected[index] |= 0x01;
      assert(lastLut(bus) && *lastLut(bus) == expected);
    } else assert(!lastLut(bus));
    const bool stickyAa = !absolute && lut == lut_grayscale_sticky;
    uint8_t sequence = lut ? 0xC7 : 0xD7;
    if (wasOn) sequence &= 0x3F;
    // Stock follows the caller: retain power for AA here, shut down for the image.
    if (!absolute) sequence &= 0xFC;
    if (stickyAa) sequence = 0xCC;
    assert(lastRegister(bus, 0x22) == sequence);
    if (stickyAa) {
      assert(lastRegister(bus, 0x03) == lut[105]);
      assert(lastRegister(bus, 0x2C) == lut[109]);
      assert(lastRegister(bus, 0x3C) == 0x80);
      for (const auto& w : bus.writes)
        if (w.command == 0x04) assert(w.bytes == Bytes(lut + 106, lut + 109));
    }
    unsigned activations = 0;
    for (const auto& w : bus.writes) {
      if (!stickyAa) assert(w.command != 0x03 && w.command != 0x04 && w.command != 0x2C);
      activations += w.command == 0x20;
    }
    assert(activations == 1);
    if (!absolute) {
      d.cleanupGrayscaleBuffers(bus, base.data());
      bus.clear();
      d.display(bus, base.data(), nullptr, RefreshMode::Fast, false);
      for (const auto& w : bus.writes)
        if (w.command == 0x26) assert(w.bytes == Bytes(48000, 0x66));
    } else assert(d._needsGrayClear && !d._isScreenOn);
  }
}

static void expectStickyAaPower(Ssd1677Driver& d) {
  EpdBus bus;
  Bytes base(48000, 0x55), lsb(48000, 0x11), msb(48000, 0x33);
  for (bool turnOff : {false, true}) {
    d.begin(bus);
    d.beginGrayscale(bus, base.data(), GrayscaleMode::Overlay, RefreshMode::Full, true);
    d.copyGrayscaleLsb(bus, lsb.data());
    d.copyGrayscaleMsb(bus, msb.data());
    assert(!d._isScreenOn);
    bus.clear();
    d.displayGray(bus, msb.data(), turnOff, nullptr, false);
    Bytes expected(lut_grayscale_sticky, lut_grayscale_sticky + 105);
    expected[12] = 0x41;
    expected[22] = 0xA9;
    expected[32] = 0x21;
    assert(lastLut(bus) && *lastLut(bus) == expected);
    assert(lastRegister(bus, 0x2C) == lut_grayscale_sticky[109]);
    std::vector<uint8_t> sequences;
    for (const auto& w : bus.writes) {
      if (w.command == 0x22) sequences.push_back(w.bytes[0]);
      assert(w.command != 0x1A);  // Sticky AA does not load a fixed temperature.
    }
    assert(sequences == std::vector<uint8_t>({0xC0, static_cast<uint8_t>(turnOff ? 0xCF : 0xCC)}));
    assert(d._isScreenOn == !turnOff && !d._customLutActive);
  }
}

// A complete frame can span an async base and a grayscale overlay. Idle
// shutdown must retain RAM/state, and the next page must remain partial.
static void expectIdlePower(Ssd1677Driver& d) {
  FreeInkDisplay frame(1, 2, 3, 4, 5, 6);
  frame.selectDriver();
  auto& bus = frame._bus;
  const Bytes fb(48000, 0xAA);
  d.begin(bus);
  d.display(bus, fb.data(), nullptr, RefreshMode::Full, false);
  bus.clear();
  assert(d.supportsAsyncDisplay());
  assert(d.displayStart(bus, fb.data(), fb.data(), RefreshMode::Fast, false));
  const uint8_t startedSequence = lastRegister(bus, 0x22);
  const bool needsShutdown = (startedSequence & 0x03) == 0;
  assert(d._isScreenOn == needsShutdown);
  assert(bus.waits == 0);
  frame._refreshPending = true;
  fakeDelayMs = 0;
  frame.powerOffBetweenFrames();
  assert(!frame._refreshPending && !d._isScreenOn && !d._needsInitialFull);
  assert(lastRegister(bus, 0x22) == (needsShutdown ? 0x03 : startedSequence));
  assert(bus.waits == (needsShutdown ? 2u : 1u));
  assert(fakeDelayMs == (needsShutdown ? 1u : 0u));  // command propagation only; never the 200 ms helper delay
  for (const auto& w : bus.writes) assert(w.command != 0x10 && w.command != 0x12);  // no sleep/reset
  bus.clear();
  frame.powerOffBetweenFrames();
  assert(bus.writes.empty());
  assert(d.displayStart(bus, fb.data(), fb.data(), RefreshMode::Fast, false));
  assert(lastRegister(bus, 0x22) == (d._cfg.piccoWaveforms ? (startedSequence | 0xC0) : startedSequence));
  d.displayFinish(bus, fb.data());
  // Reader AA powers off; RAM cleanup and repeated idle shutdown remain safe.
  d.displayGray(bus, fb.data(), false, nullptr, false);
  assert(d._isScreenOn);
  d.cleanupGrayscaleBuffers(bus, fb.data());
  frame.powerOffBetweenFrames();
  assert(!d._isScreenOn && !d._inGrayscaleMode && !d._needsInitialFull);
  d.displayGray(bus, fb.data(), false, nullptr, true);
  frame.powerOffBetweenFrames();
  assert(!d._isScreenOn && d._needsGrayClear);
}

static void expectSsd2677NightMode(Ssd2677Driver& driver) {
  for (bool reading : {false, true}) {
    for (auto mode : {FreeInkDisplay::FAST_REFRESH, FreeInkDisplay::HALF_REFRESH, FreeInkDisplay::FULL_REFRESH}) {
      FreeInkDisplay display(1, 2, 3, 4, 5, 6);
      display._driver = &driver;
      display.begin();
      driver.setReaderRefresh(reading);
      display.setInverted(true);
      std::memset(display.getFrameBuffer(), 0xCC, 48000);
      display.displayBuffer(FreeInkDisplay::FULL_REFRESH, true);
      std::memset(display.getFrameBuffer(), 0xAA, 48000);
      display._bus.clear();
      display.displayBuffer(mode, true);
      const bool fast = mode == FreeInkDisplay::FAST_REFRESH;
      const unsigned expectedFrames = 1;
      // A whole-screen change is a redraw: mode 2 in both contexts.
      const auto* lut = fast ? lut_picco_se0400_fast : lut_picco_se0400_full;
      unsigned frames = 0, activations = 0, lutLoads = 0;
      for (const auto& w : display._bus.writes) {
        if (w.command == 0x10) {
          assert(activations == frames);
          assert(w.bytes == Bytes(96000, fast ? 0x9B : 0x1B));
          ++frames;
        }
        if (w.command == 0x20) {
          assert(w.bytes == Bytes(lut, lut + 535));
          ++lutLoads;
        }
        if (w.command == 0x12) ++activations;
      }
      assert(frames == expectedFrames && activations == expectedFrames && lutLoads == expectedFrames);
      assert(Bytes(driver._old, driver._old + 48000) == Bytes(48000, 0x55));
      assert(driver._needsPowerOn);
      display.setInverted(false);
    }
  }
  driver.setReaderRefresh(false);
}

// BOOX 2959 overlays reader AA on inverted pages: masks keep their encoding,
// the retained B/W baseline keeps wire polarity, and the next page drives gray pixels.
static void expectNightGray(Ssd1677Driver& d) {
  FreeInkDisplay display(1, 2, 3, 4, 5, 6);
  display._driver = &d;
  display.begin();
  d.setReaderRefresh(true);
  display.setInverted(true);
  std::memset(display.getFrameBuffer(), 0xFF, 48000);
  display.displayBuffer(FreeInkDisplay::HALF_REFRESH, false);  // consumes the inversion clean
  if (!d._cfg.piccoWaveforms || d._cfg.fastLut == lut_picco_depg_custom_fast) {
    assert(!display.grayscaleCapabilities().supported());
    display._bus.clear();
    assert(!display.displayGrayscaleBase(GrayscaleMode::Overlay, FreeInkDisplay::FAST_REFRESH, false));
    const Bytes mask(48000, 0x55);
    display.copyGrayscaleLsbBuffers(mask.data());
    display.copyGrayscaleMsbBuffers(mask.data());
    display.writeGrayscalePlaneStrip(FreeInkDisplay::GRAY_PLANE_LSB, mask.data(), 0, 480);
    display.displayGrayBuffer(false);
    assert(display._bus.writes.empty());
    display.setInverted(false);
    if (d._cfg.fastLut == lut_picco_depg_custom_fast)
      assert(display.grayscaleCapabilities().supported());
    d.setReaderRefresh(false);
    return;
  }
  assert(display.grayscaleCapabilities().supported());
  const Bytes lsb(48000, 0x00), msb(48000, 0x03);
  uint8_t* fb = display.getFrameBuffer();
  std::memset(fb, 0xF0, 48000);
  display.displayBuffer(FreeInkDisplay::FAST_REFRESH, false);
  display._bus.clear();
  display.writeGrayscalePlaneStrip(FreeInkDisplay::GRAY_PLANE_LSB, lsb.data(), 0, 480);
  display.writeGrayscalePlaneStrip(FreeInkDisplay::GRAY_PLANE_MSB, msb.data(), 0, 480);
  display.displayGrayBuffer(false);
  bool lsbSeen = false, msbSeen = false;
  for (const auto& w : display._bus.writes) {
    if (w.command == 0x24 && w.bytes.size() == 48000) lsbSeen = w.bytes == lsb;
    if (w.command == 0x26 && w.bytes.size() == 48000) msbSeen = w.bytes == msb;
  }
  assert(lsbSeen && msbSeen && lastRegister(display._bus, 0x22) != 0);
  display.cleanupGrayscaleBuffers(fb);
  assert(fb[0] == 0xF0 && d._piccoOld[0] == 0x0F);
  std::memset(display.getFrameBuffer(), 0xFF, 48000);
  display._bus.clear();
  display.displayBuffer(FreeInkDisplay::FAST_REFRESH, false);
  const Bytes* old = nullptr;
  for (const auto& w : display._bus.writes)
    if (w.command == 0x26) old = &w.bytes;
  // (0x0F & ~0x03) | (~0x00 & 0x03): gray pixels start opposite the new target.
  assert(old && (*old)[0] == (d._cfg.fastLut == lut_picco_depg_custom_fast ? 0xFF : 0x0F));
  display.setInverted(false);
  d.setReaderRefresh(false);
}

// BOOX 2959 overlays reader AA on inverted SE0400 pages (MSB high, LSB low);
// the next page starts masked pixels opposite their target.
static void expectNightGray2677(Ssd2677Driver& s) {
  FreeInkDisplay display(1, 2, 3, 4, 5, 6);
  display._driver = &s;
  display.begin();
  s.setReaderRefresh(true);
  display.setInverted(true);
  std::memset(display.getFrameBuffer(), 0xFF, 48000);
  display.displayBuffer(FreeInkDisplay::HALF_REFRESH, false);
  assert(display.grayscaleCapabilities().supported());
  std::memset(display.getFrameBuffer(), 0xFF, 48000);
  display.displayGrayscaleBase(FreeInkDisplay::FAST_REFRESH, false);
  const Bytes lsb(48000, 0x00), msb(48000, 0xFF);
  display._bus.clear();
  display.copyGrayscaleLsbBuffers(lsb.data());
  display.copyGrayscaleMsbBuffers(msb.data());
  display.displayGrayBuffer(false);
  const Bytes* gray = nullptr;
  for (const auto& w : display._bus.writes)
    if (w.command == 0x10) gray = &w.bytes;
  assert(gray && *gray == Bytes(96000, 0xAA) && s._grayPending);  // (msb 1, lsb 0) per pixel
  std::memset(display.getFrameBuffer(), 0x00, 48000);  // logical black = wire white
  display._bus.clear();
  display.displayBuffer(FreeInkDisplay::FAST_REFRESH, false);
  const Bytes* page = nullptr;
  for (const auto& w : display._bus.writes)
    if (w.command == 0x10) page = &w.bytes;
  assert(page && *page == Bytes(96000, 0x55));  // old 0 (opposite), new 1
  display.setInverted(false);
  s.setReaderRefresh(false);
}

// SE0400 overlay AA: dark pixels (both masks) go out as code 01, the darker
// driven row of the stock gray table; light pixels (MSB only) as code 10.
static void expectDarkGray2677(Ssd2677Driver& s) {
  EpdBus bus;
  Bytes base(48000, 0xFF), lsb(48000, 0x00), msb(48000, 0x00);
  lsb[0] = 0xF0;  // 4 px dark, then 4 px light
  msb[0] = 0xFF;
  s.display(bus, base.data(), nullptr, RefreshMode::Full, false);
  s.beginGrayscale(bus, base.data(), GrayscaleMode::Overlay, RefreshMode::Fast, false);
  bus.clear();
  s.copyGrayscaleLsb(bus, lsb.data());
  s.copyGrayscaleMsb(bus, msb.data());
  const Bytes* packed = nullptr;
  for (const auto& w : bus.writes)
    if (w.command == 0x10) packed = &w.bytes;
  assert(packed && (*packed)[0] == 0x55 && (*packed)[1] == 0xAA && (*packed)[2] == 0x00);
  s.displayGray(bus, base.data(), false, nullptr, false);
  // The whole gray region is still flagged for the next B/W frame.
  assert(s._grayPending && s._lsb[0] == 0xFF);
  s.display(bus, base.data(), nullptr, RefreshMode::Full, false);
}

// SE0400 reader pages: base and gray in one activation of the rest table. A pixel
// already at its target rests; leaving the reader drives the gray pixels to the next frame.
static void expectOnePassTextAa2677(Ssd2677Driver& s) {
  EpdBus bus;
  Bytes base(48000, 0xFF), lsb(48000, 0), msb(48000, 0), next(48000, 0xFF);
  base[0] = 0x1F;  // 3 px black
  lsb[0] = 0x18;   // then 2 px gray, in the base as white
  msb[0] = 0x08;
  s.display(bus, base.data(), nullptr, RefreshMode::Full, false);
  assert(s.grayscaleCapabilities(GrayscaleMode::Overlay).base == GrayscaleBase::Separate);
  s.setReaderRefresh(true);
  assert(s.grayscaleCapabilities(GrayscaleMode::Overlay).base == GrayscaleBase::Combined);
  bus.clear();
  s.beginGrayscale(bus, base.data(), GrayscaleMode::Overlay, RefreshMode::Fast, false);
  s.copyGrayscaleLsb(bus, lsb.data());
  s.copyGrayscaleMsb(bus, msb.data());
  assert(bus.writes.empty());  // nothing presented before the combined pass
  s.displayGray(bus, base.data(), false, nullptr, false);
  unsigned refreshes = 0;
  const Bytes* packed = nullptr;
  for (const auto& w : bus.writes) {
    refreshes += w.command == 0x12;
    if (w.command == 0x10) packed = &w.bytes;
    if (w.command == 0x20) assert(w.bytes == Bytes(lut_picco_se0400_text_rest, lut_picco_se0400_text_rest + 535));
  }
  // Black and white were already there and rest (10); 2 px new gray (01); the rest of the screen rests.
  assert(refreshes == 1 && packed && (*packed)[0] == 0xA9 && (*packed)[1] == 0x6A && (*packed)[2] == 0xAA);
  assert(s._grayPending && s._lsb[0] == 0x18 && s._needsPowerOn && !s._textAa.ready());
  // The same page again: its gray is on the glass now, so every pixel rests.
  s.beginGrayscale(bus, base.data(), GrayscaleMode::Overlay, RefreshMode::Fast, false);
  s.copyGrayscaleLsb(bus, lsb.data());
  s.copyGrayscaleMsb(bus, msb.data());
  bus.clear();
  s.displayGray(bus, base.data(), false, nullptr, false);
  packed = nullptr;
  for (const auto& w : bus.writes)
    if (w.command == 0x10) packed = &w.bytes;
  assert(packed && (*packed)[0] == 0xAA && (*packed)[1] == 0xAA);
  s.cleanupGrayscaleBuffers(bus, base.data());
  s.setReaderRefresh(false);
  bus.clear();
  s.display(bus, next.data(), nullptr, RefreshMode::Fast, false);
  packed = nullptr;
  for (const auto& w : bus.writes)
    if (w.command == 0x10) packed = &w.bytes;
  // Black and gray pixels are both driven to the white target (01); white stays (11).
  assert(packed && (*packed)[0] == 0x55 && (*packed)[1] == 0x7F && !s._textBegun);
  // The first frame after a book page keeps the reader waveform.
  for (const auto& w : bus.writes)
    if (w.command == 0x20) assert(w.bytes == Bytes(lut_picco_se0400_fast, lut_picco_se0400_fast + 535));
  // A skipped pass still presents the page.
  s.setReaderRefresh(true);
  s.beginGrayscale(bus, base.data(), GrayscaleMode::Overlay, RefreshMode::Fast, false);
  bus.clear();
  s.cleanupGrayscaleBuffers(bus, base.data());
  assert(!s._textBegun && !bus.writes.empty());
  s.setReaderRefresh(false);
}

// A lab's frame (development builds) after a combined text pass: the LUT goes out as given, and nothing the
// pass left behind (the gray mask for the next B/W frame) survives it; that frame is a full one.
static void expectLabFrame2677(Ssd2677Driver& s) {
  EpdBus bus;
  Bytes base(48000, 0xFF), lsb(48000, 0), msb(48000, 0), red(48000, 0x0F), lut(535, 0x5A);
  base[0] = 0x1F;
  lsb[0] = 0x18;
  msb[0] = 0x08;
  s.display(bus, base.data(), nullptr, RefreshMode::Full, false);
  s.setReaderRefresh(true);
  s.beginGrayscale(bus, base.data(), GrayscaleMode::Overlay, RefreshMode::Fast, false);
  s.copyGrayscaleLsb(bus, lsb.data());
  s.copyGrayscaleMsb(bus, msb.data());
  s.displayGray(bus, base.data(), false, nullptr, false);
  s.setReaderRefresh(false);
  assert(s._grayPending);  // the rest pass's gray history, on the glass
  bus.clear();
  assert(s.displayLabFrame(bus, base.data(), red.data(), lut.data()));
  for (const auto& w : bus.writes)
    if (w.command == 0x20) assert(w.bytes == lut);
  assert(!s._grayPending && !s._grayMaskReady && s._needsInitialFull);
  bus.clear();
  s.display(bus, base.data(), nullptr, RefreshMode::Fast, false);
  for (const auto& w : bus.writes)
    if (w.command == 0x20) assert(w.bytes == Bytes(lut_picco_se0400_full, lut_picco_se0400_full + 535));
}

// Witch Reader's single push: the reader hint alone keeps the two-pass flow; setOnePassText() stages a
// page's planes and displayGrayFrame() sends the rest pass the combined base sends.
static void expectSinglePushRestText2677(Ssd2677Driver& s) {
  EpdBus bus;
  Bytes base(48000, 0xFF), lsb(48000, 0), msb(48000, 0), next(48000, 0xFF);
  base[0] = 0x1F;  // 3 px black
  lsb[0] = 0x18;   // then 2 px gray
  msb[0] = 0x08;
  s.display(bus, base.data(), nullptr, RefreshMode::Full, false);
  assert(s.supportsOnePassText());
  s.setReaderRefresh(true);  // a book page with one-pass text off
  assert(!s.supportsGrayFrame());
  s.setOnePassText(true);
  assert(s.supportsGrayFrame());
  for (int page = 0; page < 2; ++page) {
    bus.clear();
    s.copyGrayscaleLsb(bus, lsb.data());
    s.copyGrayscaleMsb(bus, msb.data());
    assert(bus.writes.empty());  // staged, not uploaded
    s.displayGrayFrame(bus, base.data(), RefreshMode::Fast, false);
    unsigned refreshes = 0;
    const Bytes* packed = nullptr;
    for (const auto& w : bus.writes) {
      refreshes += w.command == 0x12;
      if (w.command == 0x10) packed = &w.bytes;
      if (w.command == 0x20) assert(w.bytes == Bytes(lut_picco_se0400_text_rest, lut_picco_se0400_text_rest + 535));
    }
    assert(refreshes == 1 && packed);
    if (page == 0) {
      // Black and white rest (10), 2 px new gray (01), the rest of the screen rests.
      assert((*packed)[0] == 0xA9 && (*packed)[1] == 0x6A && (*packed)[2] == 0xAA);
    } else {
      // The same page again: its gray is on the glass, so every pixel rests.
      assert((*packed)[0] == 0xAA && (*packed)[1] == 0xAA);
    }
    assert(s._grayPending && s._lsb[0] == 0x18);
  }
  s.setOnePassText(false);
  s.setReaderRefresh(false);
  assert(!s.supportsGrayFrame());
  bus.clear();
  s.display(bus, next.data(), nullptr, RefreshMode::Fast, false);
  const Bytes* packed = nullptr;
  for (const auto& w : bus.writes)
    if (w.command == 0x10) packed = &w.bytes;
  // Black and gray pixels are both driven to the white target (01); white stays (11).
  assert(packed && (*packed)[0] == 0x55 && (*packed)[1] == 0x7F);
}

struct Sent {
  unsigned refreshes = 0;
  std::vector<const Bytes*> luts, data;
};

static Sent sent(const EpdBus& bus) {
  Sent s;
  for (const auto& w : bus.writes) {
    s.refreshes += w.command == 0x12;
    if (w.command == 0x20) s.luts.push_back(&w.bytes);
    if (w.command == 0x10) s.data.push_back(&w.bytes);
  }
  return s;
}

static bool isLut(const Bytes* sentLut, const unsigned char* lut) {
  return sentLut && *sentLut == Bytes(lut, lut + 535);
}

// The single push on a clean refresh keeps two passes: the page in B/W, then its gray as two-pass mode's overlay,
// dark (LSB) as 01 and light (MSB only) as 10. The next page's rest pass drives the overlay's gray that stays to
// the rest gray once, then lets it rest.
static void expectSinglePushCleanRefresh2677(Ssd2677Driver& s) {
  EpdBus bus;
  Bytes base(48000, 0xFF), lsb(48000, 0), msb(48000, 0);
  base[0] = 0x07;  // 3 px ink, 2 px gray (black in the B/W page, as the renderer draws them), 3 px white
  lsb[0] = 0x10;   // px 3 dark
  msb[0] = 0x18;   // px 3 dark, px 4 light
  s.display(bus, Bytes(48000, 0xFF).data(), nullptr, RefreshMode::Full, false);
  s.setReaderRefresh(true);
  s.setOnePassText(true);
  assert(s._textDark && s.supportsGrayFrame());
  const auto stage = [&]() {
    bus.clear();
    s.copyGrayscaleLsb(bus, lsb.data());
    s.copyGrayscaleMsb(bus, msb.data());
    assert(bus.writes.empty());
    bus.clear();
  };

  stage();
  s.displayGrayFrame(bus, base.data(), RefreshMode::Half, false);
  Sent out = sent(bus);
  assert(out.refreshes == 2 && out.luts.size() == 2 && out.data.size() == 2);
  assert(isLut(out.luts[0], lut_picco_se0400_full) && isLut(out.luts[1], lut_picco_se0400_gray));
  // The overlay: px 3 dark (01), px 4 light (10), everything else undriven (00).
  const Bytes& overlay = *out.data[1];
  assert(overlay[0] == 0x01 && overlay[1] == 0x80 && overlay[2] == 0x00 && overlay[3] == 0x00);
  assert(s._grayPending && s._lsb[0] == 0x18 && !s._restGrayOnGlass && memcmp(s._old, base.data(), 48000) == 0);

  // The same page, FAST: ink and white rest (10); the overlay's gray takes the rest table's gray row (01).
  stage();
  s.displayGrayFrame(bus, base.data(), RefreshMode::Fast, false);
  out = sent(bus);
  assert(out.refreshes == 1 && isLut(out.luts[0], lut_picco_se0400_text_rest));
  assert((*out.data[0])[0] == 0xA9 && (*out.data[0])[1] == 0x6A && (*out.data[0])[2] == 0xAA);
  assert(s._restGrayOnGlass);
  // And again: now every pixel rests.
  stage();
  s.displayGrayFrame(bus, base.data(), RefreshMode::Fast, false);
  out = sent(bus);
  assert(out.refreshes == 1 && (*out.data[0])[0] == 0xAA && (*out.data[0])[1] == 0xAA);

  // Without the dark plane a clean refresh still works, through the rest pass after the B/W page.
  uint8_t* const dark = s._textDark;
  s._textDark = nullptr;
  stage();
  s.displayGrayFrame(bus, base.data(), RefreshMode::Half, false);
  out = sent(bus);
  assert(out.refreshes == 2 && isLut(out.luts[0], lut_picco_se0400_full) &&
         isLut(out.luts[1], lut_picco_se0400_text_rest));
  assert(s._restGrayOnGlass);
  s._textDark = dark;

  // Two-pass mode's overlay leaves its own gray too: the first one-pass page after it drives that gray again.
  // Two-pass mode as the reader runs it: the B/W page, then the planes and the overlay.
  s.setOnePassText(false);
  s.display(bus, base.data(), nullptr, RefreshMode::Fast, false);
  s.copyGrayscaleLsb(bus, lsb.data());
  s.copyGrayscaleMsb(bus, msb.data());
  s.displayGray(bus, base.data(), false, nullptr, false);
  assert(s._grayPending && !s._restGrayOnGlass);
  s.setOnePassText(true);
  stage();
  s.displayGrayFrame(bus, base.data(), RefreshMode::Fast, false);
  out = sent(bus);
  assert(out.refreshes == 1 && (*out.data[0])[0] == 0xA9 && (*out.data[0])[1] == 0x6A);
  s.setOnePassText(false);
  s.setReaderRefresh(false);
}

static unsigned commands(const EpdBus& bus, uint8_t command) {
  unsigned n = 0;
  for (const auto& w : bus.writes) n += w.command == command;
  return n;
}

// SE0400 B/W frames defer: the start fires the waveform and returns with the history already the new frame's, the
// finish waits it out and powers off, once and without a host buffer. Whatever else reaches the controller
// finishes a refresh in flight first.
static void expectDeferredRefresh2677(Ssd2677Driver& s) {
  EpdBus bus;
  const Bytes white(48000, 0xFF), lsb(48000, 0x11), msb(48000, 0x33), red(48000, 0x0F), lut(535, 0x5A);
  Bytes page(48000, 0xFF);
  page[0] = 0x0F;
  assert(s.supportsAsyncDisplay());
  s.begin(bus);
  s.display(bus, white.data(), nullptr, RefreshMode::Full, false);
  s.setReaderRefresh(true);  // book pages power off after their refresh

  bus.clear();
  assert(s.displayStart(bus, page.data(), nullptr, RefreshMode::Fast, false));
  Sent out = sent(bus);
  assert(out.refreshes == 1 && out.data.size() == 1 && isLut(out.luts[0], lut_picco_se0400_fast));
  assert(bus.writes.back().command == 0x12 && bus.refreshWaitWriteCounts.empty());  // fired, not waited out
  assert(commands(bus, 0x02) == 0 && !s._needsPowerOn && s._refreshPending);
  assert(memcmp(s._old, page.data(), 48000) == 0 && !s._needsInitialFull);
  size_t fired = bus.writes.size();
  s.displayFinish(bus, nullptr);
  assert(bus.refreshWaitWriteCounts == std::vector<size_t>({fired}));
  assert(bus.writes.size() == fired + 1 && bus.writes.back().command == 0x02 && s._needsPowerOn && !s._refreshPending);
  s.displayFinish(bus, nullptr);  // once
  assert(bus.writes.size() == fired + 1 && bus.refreshWaitWriteCounts.size() == 1);

  // BUSY seen to drop at the start and back up at the finish: the waveform is over, nothing to wait for.
  bus.clear();
  bus.busy = true;
  assert(s.displayStart(bus, white.data(), nullptr, RefreshMode::Fast, false));
  bus.busy = false;
  s.displayFinish(bus, nullptr);
  assert(bus.refreshWaitWriteCounts.empty() && bus.writes.back().command == 0x02 && s._needsPowerOn);
  // Still down: the finish waits.
  bus.clear();
  bus.busy = true;
  assert(s.displayStart(bus, page.data(), nullptr, RefreshMode::Fast, false));
  s.displayFinish(bus, nullptr);
  assert(bus.refreshWaitWriteCounts.size() == 1 && s._needsPowerOn);
  bus.busy = false;

  // A resync asked for while the waveform runs is not undone by the finish: the next frame is a full one.
  assert(s.displayStart(bus, white.data(), nullptr, RefreshMode::Fast, false));
  s.requestResync(0);
  s.displayFinish(bus, nullptr);
  assert(s._needsInitialFull);
  bus.clear();
  s.display(bus, white.data(), nullptr, RefreshMode::Fast, false);
  assert(isLut(sent(bus).luts[0], lut_picco_se0400_full) && !s._needsInitialFull);

  // Every entry that reaches the controller waits the refresh in flight out and powers off before its first byte.
  const auto finishesFirst = [&](const char* what, auto entry) {
    bus.clear();
    assert(s.displayStart(bus, page.data(), nullptr, RefreshMode::Fast, false));
    fired = bus.writes.size();
    entry();
    if (s._refreshPending || bus.refreshWaitWriteCounts.empty() || bus.refreshWaitWriteCounts[0] != fired ||
        bus.writes.size() <= fired || bus.writes[fired].command != 0x02) {
      std::printf("a refresh in flight was not finished before %s\n", what);
      assert(false);
    }
  };
  finishesFirst("display", [&] { s.display(bus, white.data(), nullptr, RefreshMode::Fast, false); });
  finishesFirst("displayStart", [&] {
    assert(s.displayStart(bus, white.data(), nullptr, RefreshMode::Fast, false));
    assert(bus.refreshWaitWriteCounts.size() == 1);  // the first one's wait; its own is still owed
    s.displayFinish(bus, nullptr);
  });
  finishesFirst("beginGrayscale", [&] {
    s.beginGrayscale(bus, page.data(), GrayscaleMode::Overlay, RefreshMode::Fast, false);
  });
  finishesFirst("copyGrayscaleLsb", [&] { s.copyGrayscaleLsb(bus, lsb.data()); });
  finishesFirst("copyGrayscaleMsb", [&] { s.copyGrayscaleMsb(bus, msb.data()); });
  finishesFirst("displayGray", [&] { s.displayGray(bus, page.data(), false, nullptr, false); });
  finishesFirst("cleanupGrayscaleBuffers", [&] { s.cleanupGrayscaleBuffers(bus, page.data()); });
  finishesFirst("powerOffBetweenFrames", [&] { s.powerOffBetweenFrames(bus); });
  finishesFirst("displayLabFrame", [&] { assert(s.displayLabFrame(bus, page.data(), red.data(), lut.data())); });
  finishesFirst("deepSleep", [&] {
    s.deepSleep(bus);
    assert(bus.writes.back().command == 0x07);
  });
  s.begin(bus);
  s.display(bus, white.data(), nullptr, RefreshMode::Full, false);

  // A small update outside the reader keeps the rails on (mode 3): its finish waits and leaves them so.
  s.setReaderRefresh(false);
  s.display(bus, white.data(), nullptr, RefreshMode::Fast, false);  // the frame after a book page still powers off
  Bytes moved(48000, 0xFF);
  moved[0] = 0x7F;
  bus.clear();
  assert(s.displayStart(bus, moved.data(), nullptr, RefreshMode::Fast, false));
  assert(isLut(sent(bus).luts[0], lut_picco_se0400_mode3));
  s.displayFinish(bus, nullptr);
  assert(bus.refreshWaitWriteCounts.size() == 1 && commands(bus, 0x02) == 0 && !s._needsPowerOn);
  s.powerOffBetweenFrames(bus);
  assert(s._needsPowerOn);

  // Through the facade: a triggered refresh stays pending until it is collected, an async one too; deep sleep and
  // the gray cleanup with the previous buffer collect it themselves; inverted output never defers.
  FreeInkDisplay display(1, 2, 3, 4, 5, 6);
  display._driver = &s;
  display.begin();
  assert(display.supportsAsyncRefresh());
  std::memset(display.getFrameBuffer(), 0xFF, 48000);
  display.displayBuffer(FreeInkDisplay::FULL_REFRESH);
  assert(!display.isRefreshPending());
  std::memset(display.getFrameBuffer(), 0xF0, 48000);
  display._bus.clear();
  display.triggerDisplay(FreeInkDisplay::FAST_REFRESH);
  assert(display.isRefreshPending() && sent(display._bus).refreshes == 1 && display._bus.refreshWaitWriteCounts.empty());
  display.completeDisplay();
  assert(!display.isRefreshPending() && display._bus.refreshWaitWriteCounts.size() == 1);
  std::memset(display.getFrameBuffer(), 0x0F, 48000);
  display.triggerDisplayAsync(FreeInkDisplay::FAST_REFRESH);
  assert(display.isRefreshPending());
  display.finishDisplayAsync();
  assert(!display.isRefreshPending() && display._bus.refreshWaitWriteCounts.size() == 2);
  std::memset(display.getFrameBuffer(), 0xF0, 48000);
  display.triggerDisplay(FreeInkDisplay::FAST_REFRESH);
  display.cleanupGrayscaleWithPreviousBuffer();
  assert(!display.isRefreshPending() && display._bus.refreshWaitWriteCounts.size() == 3);
  std::memset(display.getFrameBuffer(), 0x0F, 48000);
  display._bus.clear();
  display.triggerDisplay(FreeInkDisplay::FAST_REFRESH);
  fired = display._bus.writes.size();
  display.deepSleep();
  assert(!display.isRefreshPending() && display._bus.refreshWaitWriteCounts == std::vector<size_t>({fired}));
  assert(display._bus.writes.back().command == 0x07);
  display.begin();
  std::memset(display.getFrameBuffer(), 0xFF, 48000);
  display.displayBuffer(FreeInkDisplay::FULL_REFRESH);
  display.setInverted(true);
  assert(!display.supportsAsyncRefresh());
  display._bus.clear();
  display.triggerDisplay(FreeInkDisplay::FAST_REFRESH);
  assert(!display.isRefreshPending() && display._bus.refreshWaitWriteCounts.size() == 1);
  display.triggerDisplayAsync(FreeInkDisplay::FAST_REFRESH);
  assert(!display.isRefreshPending() && display._bus.refreshWaitWriteCounts.size() == 2);
  display.setInverted(false);
  std::memset(display.getFrameBuffer(), 0xFF, 48000);
  display.displayBuffer(FreeInkDisplay::FULL_REFRESH);
}

// The single push defers as well. FAST: the rest pass is fired with the history already the page's, the finish
// waits and powers off. A clean refresh: the start fires the B/W page and the finish sends its gray overlay, from
// the planes staged for that page even when the next page's arrive while it paints.
static void expectDeferredSinglePush2677(Ssd2677Driver& s) {
  EpdBus bus;
  const Bytes white(48000, 0xFF), nextLsb(48000, 0xFF), nextMsb(48000, 0xFF);
  Bytes base(48000, 0xFF), lsb(48000, 0), msb(48000, 0);
  base[0] = 0x07;  // as expectSinglePushCleanRefresh2677: 3 px ink, 2 px gray, 3 px white
  lsb[0] = 0x10;
  msb[0] = 0x18;
  s.begin(bus);
  s.display(bus, white.data(), nullptr, RefreshMode::Full, false);
  s.setReaderRefresh(true);
  s.setOnePassText(true);
  assert(s._textDark && s.supportsGrayFrame());
  const auto stage = [&]() {
    s.copyGrayscaleLsb(bus, lsb.data());
    s.copyGrayscaleMsb(bus, msb.data());
    bus.clear();
  };

  stage();
  assert(s.displayGrayFrameStart(bus, base.data(), RefreshMode::Half, false));
  Sent out = sent(bus);
  assert(out.refreshes == 1 && isLut(out.luts[0], lut_picco_se0400_full) && bus.refreshWaitWriteCounts.empty());
  assert(s._refreshPending && memcmp(s._old, base.data(), 48000) == 0);
  s.copyGrayscaleLsb(bus, nextLsb.data());  // the next page's pre-render stages its planes
  out = sent(bus);
  assert(out.refreshes == 2 && out.data.size() == 2 && isLut(out.luts[1], lut_picco_se0400_gray));
  // The overlay of the page in flight: px 3 dark (01), px 4 light (10), nothing else driven.
  assert((*out.data[1])[0] == 0x01 && (*out.data[1])[1] == 0x80 && (*out.data[1])[2] == 0x00);
  assert(!s._refreshPending && s._grayPending && s._lsb[0] == 0x18 && !s._restGrayOnGlass && s._needsPowerOn);
  size_t done = bus.writes.size();
  s.displayFinish(bus, nullptr);  // nothing is left of it
  assert(bus.writes.size() == done);
  s.copyGrayscaleMsb(bus, nextMsb.data());
  assert(bus.writes.size() == done && s._textStaged == 7);  // staged for the next page, not uploaded

  // The blocking form is the two halves in one call.
  stage();
  s.displayGrayFrame(bus, base.data(), RefreshMode::Half, false);
  out = sent(bus);
  assert(out.refreshes == 2 && isLut(out.luts[0], lut_picco_se0400_full) && isLut(out.luts[1], lut_picco_se0400_gray));
  assert(!s._refreshPending && bus.refreshWaitWriteCounts.size() == 2);

  // FAST: one refresh with the rest table. The gray history is this page's from the start.
  stage();
  assert(s.displayGrayFrameStart(bus, base.data(), RefreshMode::Fast, false));
  out = sent(bus);
  assert(out.refreshes == 1 && isLut(out.luts[0], lut_picco_se0400_text_rest) && bus.refreshWaitWriteCounts.empty());
  assert(commands(bus, 0x02) == 0 && s._refreshPending);
  assert(s._grayPending && s._restGrayOnGlass && s._lsb[0] == 0x18 && memcmp(s._old, base.data(), 48000) == 0);
  size_t fired = bus.writes.size();
  s.displayFinish(bus, nullptr);
  assert(bus.refreshWaitWriteCounts == std::vector<size_t>({fired}));
  assert(bus.writes.size() == fired + 1 && bus.writes.back().command == 0x02 && s._needsPowerOn);

  // A B/W frame that arrives while the rest pass paints finishes it first, then drives its gray away.
  stage();
  assert(s.displayGrayFrameStart(bus, base.data(), RefreshMode::Fast, false));
  fired = bus.writes.size();
  s.display(bus, white.data(), nullptr, RefreshMode::Fast, false);
  assert(bus.refreshWaitWriteCounts.size() == 2 && bus.refreshWaitWriteCounts[0] == fired);
  assert(bus.writes[fired].command == 0x02 && !s._grayPending);

  // A push that starts while a B/W frame paints finishes that frame before its page copy and its codes go out.
  stage();
  assert(s.displayStart(bus, white.data(), nullptr, RefreshMode::Fast, false));
  fired = bus.writes.size();
  assert(s.displayGrayFrameStart(bus, base.data(), RefreshMode::Fast, false));
  assert(bus.refreshWaitWriteCounts == std::vector<size_t>({fired}) && bus.writes[fired].command == 0x02);
  assert(isLut(sent(bus).luts[1], lut_picco_se0400_text_rest));
  s.displayFinish(bus, nullptr);

  // A combined-base page (beginGrayscale) that arrives while a clean push paints: the push's overlay still goes
  // out, from its own planes, before the new page takes the staging over.
  stage();
  assert(s.displayGrayFrameStart(bus, base.data(), RefreshMode::Half, false));
  s.beginGrayscale(bus, white.data(), GrayscaleMode::Overlay, RefreshMode::Fast, false);
  out = sent(bus);
  assert(out.refreshes == 2 && isLut(out.luts[1], lut_picco_se0400_gray) && (*out.data[1])[0] == 0x01);
  assert(s._textBegun && s._textStaged == 0 && !s._refreshPending);
  s.cleanupGrayscaleBuffers(bus, white.data());  // the skipped pass presents its page

  // begin() resets the controller: a push in flight is gone with it, and its overlay never follows. The first
  // frame after init cleans the glass to white first (Free-Ink 634a3f8), then goes out as a FULL of its own.
  stage();
  assert(s.displayGrayFrameStart(bus, base.data(), RefreshMode::Half, false));
  s.begin(bus);
  bus.clear();
  s.display(bus, white.data(), nullptr, RefreshMode::Fast, false);
  out = sent(bus);
  assert(out.refreshes == 2 && isLut(out.luts[0], lut_picco_se0400_full) && isLut(out.luts[1], lut_picco_se0400_full));
  assert(bus.refreshWaitWriteCounts.size() == 2 && bus.refreshWaitWriteCounts[1] == bus.writes.size() - 1);
  s.setReaderRefresh(true);
  s.setOnePassText(true);

  // Without the dark plane the clean push still defers: the B/W page, then the rest pass from the finish.
  uint8_t* const dark = s._textDark;
  s._textDark = nullptr;
  stage();
  assert(s.displayGrayFrameStart(bus, base.data(), RefreshMode::Half, false));
  assert(sent(bus).refreshes == 1);
  s.displayFinish(bus, nullptr);
  out = sent(bus);
  assert(out.refreshes == 2 && isLut(out.luts[1], lut_picco_se0400_text_rest) && s._restGrayOnGlass);
  s._textDark = dark;

  // Through the facade the push stays pending until completeDisplay() collects it.
  FreeInkDisplay display(1, 2, 3, 4, 5, 6);
  display._driver = &s;
  display.begin();
  s.setReaderRefresh(true);
  s.setOnePassText(true);
  std::memset(display.getFrameBuffer(), 0xFF, 48000);
  display.displayBuffer(FreeInkDisplay::FULL_REFRESH);
  assert(display.supportsGrayFrame());
  display.copyGrayscaleLsbBuffers(lsb.data());
  display.copyGrayscaleMsbBuffers(msb.data());
  std::memcpy(display.getFrameBuffer(), base.data(), 48000);
  display._bus.clear();
  display.triggerGrayscaleFrame(FreeInkDisplay::HALF_REFRESH);
  assert(display.isRefreshPending() && sent(display._bus).refreshes == 1);
  display.completeDisplay();
  assert(!display.isRefreshPending() && sent(display._bus).refreshes == 2);
  // A host asks isRefreshPending() before it powers the panel off or lets the chip sleep: the answer is yes from the
  // trigger until the finish has returned, through every wait in it: the page's waveform and power off, the overlay's
  // power on, waveform and power off.
  display.copyGrayscaleLsbBuffers(lsb.data());
  display.copyGrayscaleMsbBuffers(msb.data());
  unsigned waitsSeen = 0, waitsPending = 0;
  display._bus.onWait = [&] {
    ++waitsSeen;
    waitsPending += display.isRefreshPending();
  };
  display.triggerGrayscaleFrame(FreeInkDisplay::HALF_REFRESH);
  assert(display.isRefreshPending());
  waitsSeen = waitsPending = 0;
  display.completeDisplay();
  assert(waitsSeen == 5 && waitsPending == 5 && !display.isRefreshPending());
  display._bus.onWait = nullptr;
  display.copyGrayscaleLsbBuffers(lsb.data());
  display.copyGrayscaleMsbBuffers(msb.data());
  display._bus.clear();
  display.triggerGrayscaleFrame(FreeInkDisplay::FAST_REFRESH);
  assert(display.isRefreshPending() && isLut(sent(display._bus).luts[0], lut_picco_se0400_text_rest));
  assert(display._bus.refreshWaitWriteCounts.empty());
  display.deepSleep();
  assert(!display.isRefreshPending() && display._bus.refreshWaitWriteCounts.size() == 1);
  assert(display._bus.writes.back().command == 0x07);
  s.setOnePassText(false);
  s.setReaderRefresh(false);
  s.begin(bus);
}

// DEPG0397 reader pages: one combined activation, no separate B/W base. Other
// SSD1677 panels keep the two-pass overlay.
static void expectOnePassTextAa(Ssd1677Driver& d, bool expected) {
  EpdBus bus;
  Bytes base(48000, 0xFF), lsb(48000, 0), msb(48000, 0), next(48000, 0xFF);
  base[0] = 0x1F;  // 3 px black
  lsb[0] = 0x18;   // then 2 px gray, in the base as white
  msb[0] = 0x08;
  d.display(bus, base.data(), nullptr, RefreshMode::Full, false);
  d.setReaderRefresh(true);
  const auto caps = d.grayscaleCapabilities(GrayscaleMode::Overlay);
  assert((caps.base == GrayscaleBase::Combined) == expected);
  if (!expected) {
    d.setReaderRefresh(false);
    return;
  }
  bus.clear();
  d.beginGrayscale(bus, base.data(), GrayscaleMode::Overlay, RefreshMode::Fast, false);
  d.copyGrayscaleLsb(bus, lsb.data());
  d.copyGrayscaleMsb(bus, msb.data());
  assert(bus.writes.empty());  // nothing presented before the combined pass
  d.displayGray(bus, base.data(), false, nullptr, false);
  unsigned activations = 0;
  for (const auto& w : bus.writes) activations += w.command == 0x20;
  assert(activations == 1 && lastRegister(bus, 0x22) == 0xCC && d._textAa.onPanel());
  // The panel's own table, with its voltage tail, not the X4 Pro one.
  assert(*lastLut(bus) == Bytes(lut_picco_depg_text, lut_picco_depg_text + 105));
  assert(lastRegister(bus, 0x2C) == lut_picco_depg_text[109]);
  d.cleanupGrayscaleBuffers(bus, base.data());
  // Leaving the reader: gray pixels are driven to the new page, black/white are differential.
  d.setReaderRefresh(false);
  bus.clear();
  d.display(bus, next.data(), nullptr, RefreshMode::Fast, false);
  const Bytes* red = nullptr;
  for (const auto& w : bus.writes)
    if (w.command == 0x26) red = &w.bytes;
  assert(red && (*red)[0] == 0x07 && (*red)[1] == 0xFF);
  assert(lastRegister(bus, 0x22) == 0x0C);  // stays a fast refresh
}

// Every pixel, bit by bit: the drive is the page's state unless the glass already holds it. Gray that
// stays gray rests only when the caller says gray rests.
static void expectRestTextDrives() {
  uint32_t seed = 1;
  const auto next = [&seed]() { return static_cast<uint8_t>((seed = seed * 1103515245u + 12345u) >> 16); };
  for (int round = 0; round < 512; ++round) {
    const uint8_t page = next(), pageGray = next(), glass = next(), glassGray = next();
    const bool grayRests = round & 1;
    const RestTextDrives d = restTextDrives(page, pageGray, glass, glassGray, grayRests);
    for (int bit = 0; bit < 8; ++bit) {
      const auto at = [bit](uint8_t v) { return (v >> bit) & 1; };
      enum { White, Black, Gray } target = at(pageGray) ? Gray : at(page) ? White : Black,
                                  held = at(glassGray) ? Gray : at(glass) ? White : Black;
      const bool rests = target == held && (target != Gray || grayRests);
      assert(at(d.rest) + at(d.toWhite) + at(d.toGray) + at(d.toBlack) == 1);
      assert(at(d.rest) == rests);
      assert(at(d.toWhite) == (!rests && target == White));
      assert(at(d.toGray) == (!rests && target == Gray));
      assert(at(d.toBlack) == (!rests && target == Black));
    }
  }
}

// 16-level image, three draw passes built at runtime: they match the tables
// picco-gray-cal builds byte for byte; a clean image starts from two full
// clears; leaving gives every level its exact drive back; a fast turn is one
// dissolve plus the gray passes, with a full turn every scrubEvery.
static void expectGray16Image(Ssd2677Driver& s) {
  using Lut = const unsigned char*;
  auto lutsOf = [](const EpdBus& bus) {
    std::vector<Bytes> out;
    for (const auto& w : bus.writes)
      if (w.command == 0x20) out.push_back(w.bytes);
    return out;
  };
  auto framesOf = [](const EpdBus& bus) {
    std::vector<Bytes> out;
    for (const auto& w : bus.writes)
      if (w.command == 0x10) out.push_back(w.bytes);
    return out;
  };
  auto asBytes = [](Lut l) { return Bytes(l, l + 535); };
  auto codeAt = [](const Bytes& frame, uint32_t pixel) { return (frame[pixel / 4] >> (6 - 2 * (pixel % 4))) & 3; };
  auto levelAt = [](int x) { return static_cast<uint8_t>((x / 50) % 16); };  // every row: all 16 levels
  Bytes levels(400 * 480);
  for (int y = 0; y < 480; ++y)
    for (int x = 0; x < 800; x += 2) levels[y * 400 + x / 2] = static_cast<uint8_t>(levelAt(x) << 4 | levelAt(x + 1));
  Bytes reversed(levels.size());
  for (size_t i = 0; i < levels.size(); ++i)
    reversed[i] = static_cast<uint8_t>((15 - (levels[i] >> 4)) << 4 | (15 - (levels[i] & 15)));
  auto levelIn = [](const Bytes& img, uint32_t x) { return static_cast<uint8_t>((img[x / 2] >> ((x & 1) ? 0 : 4)) & 15); };
  const Bytes page(48000, 0xFF);

  EpdBus bus;
  assert(s.supportsGray16Image());
  assert(s.displayGray16Image(bus, levels.data(), RefreshMode::Half, true));
  // The erase and deep drives match picco-gray-cal's tables (its doses: below, under runtime doses).
  assert(Bytes(s._lutDissolve, s._lutDissolve + 535) == asBytes(lut_picco_se0400_gray16_ref_dissolve));
  assert(Bytes(s._lutDeep, s._lutDeep + 535) == asBytes(lut_picco_se0400_gray16_ref_deep));
  // Clean image: two clears, deep, coarse, pull.
  assert(lutsOf(bus) == std::vector<Bytes>({asBytes(lut_picco_se0400_full), asBytes(lut_picco_se0400_full),
                                            asBytes(s._lutDeep), asBytes(s._lutCoarse), asBytes(s._lutPull)}));
  {
    const auto f = framesOf(bus);
    assert(f.size() == 5 && f[0] == Bytes(96000, 0xFF) && f[1] == Bytes(96000, 0xFF));
    for (uint32_t y : {0u, 479u})
      for (uint32_t x = 0; x < 800; ++x) {
        const uint8_t l = levelAt(x);
        assert(codeAt(f[2], y * 800 + x) == s._g16.deep[l] && codeAt(f[3], y * 800 + x) == s._g16.coarse[l] &&
               codeAt(f[4], y * 800 + x) == s._g16.pull[l]);
      }
  }
  assert(lastRegister(bus, 0x02) == 0x00 && bus.writes.back().command == 0x02);  // powered off at the end
  assert(s._gray16OnGlass);
  // Level 0 is white: undriven in every pass, so the page around an image stays put.
  assert(s._g16.deep[0] == 3 && s._g16.coarse[0] == 3 && s._g16.pull[0] == 3 && !s._g16.ink[0]);

  // Leaving: deep give-back (if any), coarse and fine give-back toward white, then a full refresh.
  bus.clear();
  s.display(bus, page.data(), nullptr, RefreshMode::Fast, false);
  {
    std::vector<Bytes> want;
    if (s._g16.useBalanceDeep) want.push_back(asBytes(lut_picco_se0400_gray16_deep_balance));
    want.push_back(asBytes(lut_picco_se0400_gray16_balance));
    want.push_back(asBytes(lut_picco_se0400_gray16_balance_fine));
    want.push_back(asBytes(lut_picco_se0400_full));
    assert(lutsOf(bus) == want);
  }
  assert(!s._gray16OnGlass);
  // Exact: every level nets to zero over image + exit (black positive).
  for (int l = 0; l < 16; ++l) {
    const int net = ssd2677LutDose(s._lutDeep, s._g16.deep[l] + 1) + ssd2677LutDose(s._lutCoarse, s._g16.coarse[l] + 1) +
                    ssd2677LutDose(s._lutPull, s._g16.pull[l] + 1) +
                    ssd2677LutDose(lut_picco_se0400_gray16_deep_balance, s._g16.balanceDeep[l] + 1) +
                    ssd2677LutDose(lut_picco_se0400_gray16_balance, s._g16.balanceCoarse[l] + 1) +
                    ssd2677LutDose(lut_picco_se0400_gray16_balance_fine, s._g16.balanceFine[l] + 1);
    assert(net == 0);
  }
  for (Lut l : {lut_picco_se0400_gray16_deep_balance, lut_picco_se0400_gray16_balance,
                lut_picco_se0400_gray16_balance_fine})
    assert(ssd2677LutDose(l, 4) == 0);  // table 4 = code 11: undriven
  for (const uint8_t* l : {s._lutDeep, s._lutCoarse, s._lutPull, s._lutDissolve}) assert(ssd2677LutDose(l, 4) == 0);
  // Dissolve: code 01 erases (white only), 10 is the deep black, 00 both.
  assert(ssd2677LutDose(s._lutDissolve, 2) == -kGray16DefaultErase && ssd2677LutDose(s._lutDissolve, 3) == kGray16Deep &&
         ssd2677LutDose(s._lutDissolve, 1) == kGray16Deep - kGray16DefaultErase);

  // Fast turns: one dissolve, then the gray passes; a full turn after scrubEvery.
  {
    auto hasFull = [&](const EpdBus& b) {
      for (const auto& l : lutsOf(b))
        if (l == asBytes(lut_picco_se0400_full)) return true;
      return false;
    };
    assert(s.setGray16Turn(2));
    bus.clear();
    assert(s.displayGray16Image(bus, levels.data(), RefreshMode::Fast, true));  // nothing up yet: full path
    assert(hasFull(bus));
    for (int turn = 0; turn < 2; ++turn) {
      const Bytes& next = turn ? levels : reversed;
      const Bytes& prev = turn ? reversed : levels;
      bus.clear();
      assert(s.displayGray16Image(bus, next.data(), RefreshMode::Fast, true));
      assert(!hasFull(bus));
      assert(lutsOf(bus) ==
             std::vector<Bytes>({asBytes(s._lutDissolve), asBytes(s._lutCoarse), asBytes(s._lutPull)}));
      const Bytes dissolve = framesOf(bus)[0];
      for (uint32_t x = 0; x < 800; ++x) {
        const bool ink = s._g16.ink[levelIn(prev, x)], deep = s._g16.deep[levelIn(next, x)] == 2;
        assert(codeAt(dissolve, x) == (ink ? (deep ? 0 : 1) : (deep ? 2 : 3)));
      }
    }
    bus.clear();
    assert(s.displayGray16Image(bus, levels.data(), RefreshMode::Fast, true));  // third turn: the scrub
    assert(hasFull(bus));
    assert(s.setGray16Turn(0));
  }

  // Two-step 16-level text: base (dark levels black) first, then the passes;
  // after a text page the base is one cross-dissolve from it.
  {
    auto planeOf = [&](const Bytes& img, bool (*want)(Ssd2677Driver&, uint8_t)) {
      Bytes plane(48000, 0xFF);  // 1 bpp, cleared bit = marked
      for (uint32_t y = 0; y < 480; ++y)
        for (uint32_t x = 0; x < 800; ++x)
          if (want(s, levelIn(img, y * 800 + x)))
            plane[y * 100 + x / 8] &= static_cast<uint8_t>(~(0x80 >> (x & 7)));
      return plane;
    };
    auto isDeep = [](Ssd2677Driver& d, uint8_t l) { return d._g16.deep[l] == 2; };
    auto isSoft = [](Ssd2677Driver& d, uint8_t l) { return l == d._textSoftLevel; };
    bus.clear();
    s.display(bus, page.data(), nullptr, RefreshMode::Full, false);
    assert(s.setGray16Turn(2));  // turns since the last clean one: B/W 1, dissolve 2, then clean
    // On the design's paths, which have a soft level (the defaults need not); doses first, as the design's paths do
    // not fit the defaults' doses.
    assert(s.setGray16Passes(bus, kGray16DesignBlack, kGray16DesignWhite, kGray16DefaultErase));
    assert(s.setGray16Levels(bus, kGray16DesignPaths));
    assert(s._textSoftLevel == 5);  // deep black, then the largest pull-back
    // The dissolve: 01 erases, 10 lays deep black, 00 deep black then the
    // in-turn pull-back, 11 undriven.
    assert(ssd2677LutDose(s._lutCross, 2) == -kGray16DefaultErase);
    assert(ssd2677LutDose(s._lutCross, 3) == kGray16Deep);
    assert(ssd2677LutDose(s._lutCross, 1) == kGray16Deep - kGray16DesignWhite[2]);
    assert(ssd2677LutDose(s._lutCross, 4) == 0);

    // Turn 1, nothing up yet: the B/W turn, returning while the panel draws.
    const Bytes base1 = planeOf(levels, isDeep), soft1 = planeOf(levels, isSoft);
    bus.clear();
    assert(s.beginGray16Text(bus, base1.data(), soft1.data()));
    // The B/W turn goes through display(), which this fork defers with startRefresh() (_refreshPending);
    // settle() ends either kind.
    assert(!s._textDissolved && (s._refreshInFlight || s._refreshPending));
    assert(framesOf(bus).size() == 1);
    assert(s.finishGray16Text(bus, levels.data(), true));
    assert(lutsOf(bus).size() == 4);  // base, deep, coarse, pull
    assert(lutsOf(bus)[1] == asBytes(s._lutDeep) && lutsOf(bus)[3] == asBytes(s._lutPull));
    assert(bus.writes.back().command == 0x02 && s._refreshInFlight);  // power-off not waited for
    assert(s._textOnGlass);

    // Turn 2: one cross-dissolve from turn 1's levels; no deep pass.
    const Bytes base2 = planeOf(reversed, isDeep), soft2 = planeOf(reversed, isSoft);
    bus.clear();
    assert(s.beginGray16Text(bus, base2.data(), soft2.data()));
    assert(bus.waits >= 1);  // turn 1's power-off settled before the upload
    assert(s._textDissolved && lutsOf(bus) == std::vector<Bytes>({asBytes(s._lutCross)}));
    {
      const Bytes cross = framesOf(bus)[0];
      for (uint32_t y : {0u, 479u})
        for (uint32_t x = 0; x < 800; ++x) {
          const uint32_t p = y * 800 + x;
          const uint8_t oldL = levelIn(levels, p), newL = levelIn(reversed, p);
          const bool deep = s._g16.deep[newL] == 2, soft = newL == s._textSoftLevel;
          assert(codeAt(cross, p) == (deep ? (soft ? 0 : 2) : (oldL ? 1 : 3)));
        }
    }
    assert(s.finishGray16Text(bus, reversed.data(), true));
    assert(lutsOf(bus) == std::vector<Bytes>({asBytes(s._lutCross), asBytes(s._lutCoarse), asBytes(s._lutPull)}));
    {
      const Bytes pull = framesOf(bus)[2];
      for (uint32_t x = 0; x < 800; ++x) {
        const uint8_t l = levelIn(reversed, x);
        assert(codeAt(pull, x) == (l == s._textSoftLevel ? 3 : s._g16.pull[l]));
      }
    }

    // Turn 3: the clean turn (setGray16Turn(2)), full refresh then deep pass.
    bus.clear();
    assert(s.beginGray16Text(bus, base1.data(), soft1.data()));
    assert(!s._textDissolved && lutsOf(bus)[0] == asBytes(lut_picco_se0400_full));
    assert(s.finishGray16Text(bus, levels.data(), true));

    // A B/W frame ends the text page; finishGray16Text without a base refuses.
    bus.clear();
    s.display(bus, page.data(), nullptr, RefreshMode::Fast, true);
    assert(!s._textOnGlass && !s.finishGray16Text(bus, levels.data(), true));
    assert(s.setGray16Turn(0));
    assert(s.setGray16Levels(bus, kGray16DefaultPaths));
    assert(s.setGray16Passes(bus, kGray16DefaultBlack, kGray16DefaultWhite, kGray16DefaultErase));
    assert(s._textSoftLevel == 0xFF);  // the defaults have none: the pull pass pulls every deep level back
    assert(s.displayGray16Image(bus, levels.data(), RefreshMode::Half, true));  // the image the checks below start from
  }

  // Runtime doses and paths: validated against the give-back range; an image
  // on the glass is balanced with the old table before anything changes.
  {
    const uint8_t black[3] = {3, 6, 10}, white[3] = {1, 2, 3};
    const uint8_t whiteTooStrong[3] = {1, 2, 11};  // level 1 (black 10, white 11) would net below zero
    assert(!s.setGray16Passes(bus, black, whiteTooStrong, 16));
    assert(s._gray16OnGlass);  // refused before touching the glass
    bus.clear();
    assert(s.setGray16Passes(bus, black, white, 16));
    assert(!s._gray16OnGlass && !lutsOf(bus).empty());  // the default image was given back first
    assert(ssd2677LutDose(s._lutCoarse, 1 + 2) == 6 && ssd2677LutDose(s._lutPull, 1 + 0) == -3);  // codes 10, 00
    assert(ssd2677LutDose(s._lutDissolve, 2) == -16);
    const uint8_t tooMuch[3] = {3, 6, 64};
    assert(!s.setGray16Passes(bus, tooMuch, white, 16));
    uint8_t paths[16][3];
    memcpy(paths, kGray16DefaultPaths, sizeof(paths));
    paths[5][0] = 0, paths[5][1] = 3, paths[5][2] = 0;  // white with nothing to pull back: nets below zero
    assert(!s.setGray16Levels(bus, paths));
    paths[5][1] = 0, paths[5][2] = 1;  // deep black alone
    assert(s.setGray16Levels(bus, paths));
    assert(s._g16.deep[5] == 2 && s._g16.coarse[5] == 3 && s._g16.pull[5] == 3);
    assert(s._textSoftLevel == 0xFF);  // text's soft level follows: no level is deep black plus the largest pull-back
    // The runtime builder reproduces picco-gray-cal's tables for its design doses and paths.
    assert(s.setGray16Levels(bus, kGray16DesignPaths));
    assert(s.setGray16Passes(bus, kGray16DesignBlack, kGray16DesignWhite, kGray16DefaultErase));
    assert(Bytes(s._lutCoarse, s._lutCoarse + 535) == asBytes(lut_picco_se0400_gray16_ref_coarse));
    assert(Bytes(s._lutPull, s._lutPull + 535) == asBytes(lut_picco_se0400_gray16_ref_pull));
    // Back to the defaults; paths first, as the defaults' doses do not fit the design's paths.
    assert(s.setGray16Levels(bus, kGray16DefaultPaths));
    assert(s.setGray16Passes(bus, kGray16DefaultBlack, kGray16DefaultWhite, kGray16DefaultErase));
  }

  // No PSRAM for the level copy: refused before anything reaches the panel.
  Ssd2677Driver fresh;
  bus.clear();
  fakeHeapCapsFail = true;
  assert(!fresh.displayGray16Image(bus, levels.data(), RefreshMode::Half, true));
  fakeHeapCapsFail = false;
  assert(bus.writes.empty());
}

// Ported from Free-Ink 10b7ae5 (Julia Nguyen): the table-driven packPixels against the per-bit reference.
// The 8-bit canvas on top of the 16-level image (this fork): white is level 0, black level 15, a flat gray dithers to
// levels whose mean L* sits on the gray's own, mapped onto the panel's measured 7.9..70.6; the canvas goes up as the
// 16-level image and is packed in place.
static void expectGray8Canvas(Ssd2677Driver& s) {
  static constexpr double kL[16] = {70.6, 64.9, 61.4, 57.2, 54.4, 49.1, 42.6, 38.3,
                                    33.6, 25.8, 23.8, 18.7, 16.0, 13.4, 11.6, 7.9};
  assert(s.grayLevels() == 16);
  {
    Bytes c(8 * 2, 0xFF);
    Ssd2677Driver::canvasToGray16(c.data(), 8, 2);
    for (int i = 0; i < 8; ++i) assert(c[i] == 0x00);
    Bytes b(8 * 2, 0x00);
    Ssd2677Driver::canvasToGray16(b.data(), 8, 2);
    for (int i = 0; i < 8; ++i) assert(b[i] == 0xFF);
  }
  for (const int gray : {64, 128, 192}) {
    const int w = 64, h = 64;
    Bytes c(w * h, static_cast<uint8_t>(gray));
    Ssd2677Driver::canvasToGray16(c.data(), w, h);
    double sum = 0;
    for (int i = 0; i < w * h / 2; ++i) sum += kL[c[i] >> 4] + kL[c[i] & 15];
    const double mean = sum / (w * h);
    const double cl = gray / 255.0, y = cl <= 0.04045 ? cl / 12.92 : std::pow((cl + 0.055) / 1.055, 2.4);
    const double lstar = 116.0 * std::cbrt(y) - 16.0, want = 7.9 + 62.7 * lstar / 100.0;
    assert(std::fabs(mean - want) < 1.0);
  }
  uint16_t stride = 0;
  uint8_t* canvas = s.borrowGray8Canvas(&stride);
  assert(canvas && stride == 800);
  memset(canvas, 0xFF, 800 * 480);
  for (int x = 0; x < 800; ++x) canvas[x] = 0x00;  // a black first row
  EpdBus bus;
  assert(s.displayGray8Canvas(bus, RefreshMode::Half, true));
  assert(s._gray16OnGlass);
  // Nothing lent: nothing drawn, and the host is told so.
  Ssd2677Driver fresh;
  bus.clear();
  assert(!fresh.displayGray8Canvas(bus, RefreshMode::Half, true));
  assert(bus.writes.empty());
  for (int i = 0; i < 400; ++i) assert(s._gray16[i] == 0xFF);
  for (int i = 400; i < 400 * 480; ++i) assert(s._gray16[i] == 0x00);
}

static void expectPackedPairs(Ssd2677Driver& driver) {
  const size_t size = driver.geometry().bufferSize;
  Bytes high(size), low(size);
  for (bool exclusive : {false, true}) {
    for (unsigned start = 0; start < 65536; start += size) {
      for (size_t i = 0; i < size; ++i) {
        high[i] = (start + i) >> 8;
        low[i] = start + i;
      }
      EpdBus bus;
      driver.sendPacked(bus, high.data(), low.data(), exclusive);
      assert(bus.writes.size() == 1 && bus.writes[0].command == 0x10);
      const auto& packed = bus.writes[0].bytes;
      assert(packed.size() == size * 2);
      for (size_t i = 0; i < size; ++i) {
        uint16_t expected = 0;
        const uint8_t hi = exclusive ? high[i] & ~low[i] : high[i];
        for (int bit = 7; bit >= 0; --bit)
          expected = (expected << 2) | (((hi >> bit) & 1) << 1) | ((low[i] >> bit) & 1);
        assert(packed[i * 2] == expected >> 8 && packed[i * 2 + 1] == (expected & 255));
      }
    }
  }
}

int main(int argc, char** argv) {
  expectRestTextDrives();
  const char* which = argc > 1 ? argv[1] : "yrd";
  static const uint8_t yrd[10] = {0xCA, 0xFE, 0x00, 0x16, 0x77, 0x02, 0x10, 0x01, 0x00, 0x91};
  static const uint8_t depg[10] = {0xCA, 0xFE, 0x00, 0x16, 0x77, 0x00, 0x08, 0x01, 0x00, 0x4E};
  static const uint8_t opm[10] = {'W', '0', '4', '0', 'B', '3', 0, 0, 0, 0};
  static const uint8_t none[10] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  const uint8_t* id = !strcmp(which, "depg")      ? depg
                      : !strcmp(which, "opm")     ? opm
                      : !strcmp(which, "unknown") ? none
                                                  : yrd;
  memcpy(fakePanelId, id, 10);
  const auto checkController = [](const char* expected) {
    FreeInkDisplay display(1, 2, 3, 4, 5, 6);
    const int before = fakePanelCommand;
    assert(display.getControllerName() == nullptr && fakePanelCommand == before);
    assert(display.getPanelName() == nullptr && fakePanelCommand == before);
    display.selectDriver();
    assert(!strcmp(display.getControllerName(), expected));
    const int after = fakePanelCommand;
    assert(!strcmp(display.getControllerName(), expected) && fakePanelCommand == after);
  };
  if (!strcmp(which, "se0400")) {
    static const uint8_t rev[3] = {0x07, 0x01, 0x01};
    memcpy(fakePanelId, none, 10);
    memcpy(fakePanelRev, rev, 3);
    checkController("SSD2677");
    assert(piccoProbePanel() == PiccoSe0400nqw47);
    assert(!strcmp(FreeInkDisplay(1, 2, 3, 4, 5, 6).getPanelName(), "SE0400NQW47"));
    auto& s = static_cast<Ssd2677Driver&>(ssd2677Driver());
    assert(s.busyPolarity() == BusyPolarity::ActiveLow);
    EpdBus bus;
    s.begin(bus);
    expectPackedPairs(s);
    assert(lastRegister(bus, 0x00) == 0x27 && lastRegister(bus, 0xE9) == 0x01);
    if (argc > 2 && !strcmp(argv[2], "textaa")) {
      expectOnePassTextAa2677(s);
      expectLabFrame2677(s);
      expectSinglePushRestText2677(s);
      expectSinglePushCleanRefresh2677(s);
      expectDeferredSinglePush2677(s);
      std::printf("Picco se0400: one-pass text AA, lab frame, single push, its clean refresh and its deferral passed\n");
      return 0;
    }
    // Stock reader table: 24-frame drives, 1-frame top-ups. Text table: black, gray, held white, white.
    const int stockDose[] = {1, -24, 24, -1}, textDose[] = {24, 15, -1, -24};
    for (unsigned t = 0; t < 4; ++t) {
      assert(ssd2677LutDose(lut_picco_se0400_fast, t + 1) == stockDose[t]);
      assert(ssd2677LutDose(lut_picco_se0400_text, t + 1) == textDose[t]);
    }
    // Picco's final init command has no per-command wait: keep the final
    // completion wait as well as the reset wait.
    assert(bus.busyWaitWriteCounts == std::vector<size_t>({0, 12}));
    Bytes oldFb(48000, 0xFF), newFb(48000, 0xFF);
    newFb[0] = 0x0F;  // left 4 px black, right 4 px white; old all white
    bus.clear();
    s.display(bus, newFb.data(), oldFb.data(), RefreshMode::Fast, false);
    // First paint after begin(): a clean to white with every pixel taken as
    // black (01), then the frame as a FULL from white.
    const Bytes* data = nullptr;
    unsigned frames = 0;
    for (const auto& w : bus.writes)
      if (w.command == 0x10) {
        if (++frames == 1) assert(w.bytes == Bytes(96000, 0x55));
        data = &w.bytes;
      }
    assert(frames == 2 && data && data->size() == 96000);
    assert((*data)[0] == 0xAA && (*data)[1] == 0xFF);  // white->black, white->white
    const Bytes* lut = nullptr;
    for (const auto& w : bus.writes)
      if (w.command == 0x20) lut = &w.bytes;
    assert(lut && *lut == Bytes(lut_picco_se0400_full, lut_picco_se0400_full + 535));
    assert(lastRegister(bus, 0x12) == 0x00);
    bus.clear();
    s.display(bus, newFb.data(), oldFb.data(), RefreshMode::Fast, true);
    for (const auto& w : bus.writes)
      if (w.command == 0x20) lut = &w.bytes;
    assert(*lut == Bytes(lut_picco_se0400_mode3, lut_picco_se0400_mode3 + 535));
    assert(!s._needsPowerOn);
    for (const auto& w : bus.writes) assert(w.command != 0x02);
    // A second UI update reuses the powered panel, even when the host requests off.
    bus.clear();
    s.display(bus, newFb.data(), nullptr, RefreshMode::Fast, true);
    unsigned uiRefreshes = 0;
    for (const auto& w : bus.writes) {
      assert(w.command != 0x04 && w.command != 0x02);
      if (w.command == 0x12) ++uiRefreshes;
    }
    assert(uiRefreshes == 1 && !s._needsPowerOn);
    bus.clear();
    s.powerOffBetweenFrames(bus);
    assert(lastRegister(bus, 0x02) == 0 && s._needsPowerOn);
    s.setReaderRefresh(true);
    bus.clear();
    s.display(bus, newFb.data(), oldFb.data(), RefreshMode::Fast, true);
    data = nullptr;
    for (const auto& w : bus.writes) {
      if (w.command == 0x20) assert(w.bytes == Bytes(lut_picco_se0400_fast, lut_picco_se0400_fast + 535));
      if (w.command == 0x10) data = &w.bytes;
    }
    // Reader mode retains unchanged pixels without the UI black top-up.
    assert(data && data->size() == 96000 && (*data)[0] == 0x00 && (*data)[1] == 0xFF);
    // Reader refreshes power down even when the host requests retention.
    bus.clear();
    s.display(bus, newFb.data(), nullptr, RefreshMode::Fast, false);
    assert(lastRegister(bus, 0x02) == 0 && s._needsPowerOn);
    s.setReaderRefresh(false);
    assert(lastRegister(bus, 0x02) == 0x00);
    for (unsigned uiFrame = 0; uiFrame < 2; ++uiFrame) {
      bus.clear();
      s.display(bus, newFb.data(), nullptr, RefreshMode::Fast, true);
      data = nullptr;
      lut = nullptr;
      for (const auto& w : bus.writes) {
        if (w.command == 0x10) data = &w.bytes;
        if (w.command == 0x20) lut = &w.bytes;
      }
      // The first frame after a book page still uses the reading waveform.
      const auto* expected = uiFrame == 0 ? lut_picco_se0400_fast : lut_picco_se0400_mode3;
      assert(lut && *lut == Bytes(expected, expected + 535));
      assert(data && data->size() == 96000 && (*data)[1] == 0xFF);
      assert(s._needsPowerOn == (uiFrame == 0));
    }

    // Regression (single-buffer ghosting): the host builds with
    // EINK_DISPLAY_SINGLE_BUFFER_MODE, so `prev` is ALWAYS null. The driver must
    // use its OWN retained old plane — seeded white in begin (stock's 0xFF
    // obj+0x1020 fill, FUN_4200b07c), then updated to each displayed frame — or
    // every incremental update sees old==new and moves nothing (the whole screen
    // ghosts). Paint black (full), then white (fast), both with prev=null: the
    // fast frame's old plane must be the RETAINED black, giving (old=0,new=1)=0b01
    // => 0x55 bytes. The old==new bug produced 0xFF.
    s.begin(bus);  // re-seeds _old = white, arms the forced initial full
    Bytes black(48000, 0x00), white(48000, 0xFF);
    s.display(bus, black.data(), nullptr, RefreshMode::Full, false);  // _old := black
    bus.clear();
    s.display(bus, white.data(), nullptr, RefreshMode::Fast, false);  // old must = retained black
    data = nullptr;
    for (const auto& w : bus.writes)
      if (w.command == 0x10) data = &w.bytes;
    assert(data && data->size() == 96000);
    assert((*data)[0] == 0x55 && (*data)[100] == 0x55 && (*data)[95999] == 0x55);
    assert(s._needsPowerOn);  // a whole-screen UI change is a mode 2 redraw and powers off
    {
      // Light UI redraw tops up only bytes that changed; static black text is left alone.
      Bytes half(48000, 0x00);
      std::fill(half.begin() + 24000, half.end(), 0xFF);
      s.display(bus, black.data(), nullptr, RefreshMode::Full, false);
      bus.clear();
      s.display(bus, half.data(), nullptr, RefreshMode::Fast, false);
      const Bytes* frame = nullptr;
      for (const auto& w : bus.writes)
        if (w.command == 0x10) frame = &w.bytes;
      assert(frame && (*frame)[0] == 0x00 && (*frame)[48000] == 0x55);
    }
    // Overlay AA: LSB mask -> low bit, MSB mask -> high bit, slot-5 LUT.
    assert(s.grayscaleCapabilities(GrayscaleMode::Overlay).supported());
    assert(s.grayscaleCapabilities(GrayscaleMode::Absolute).supported());
    // Black, dark, light, white, repeated across the complete slot-4 frame.
    Bytes imageLsb(48000, 0x55), imageMsb(48000, 0x33);
    bus.clear();
    s.beginGrayscale(bus, white.data(), GrayscaleMode::Absolute, RefreshMode::Half, false);
    s.copyGrayscaleLsb(bus, imageLsb.data());
    s.copyGrayscaleMsb(bus, imageMsb.data());
    assert(bus.writes.empty());
    s.requestResync(1);  // the facade cancels a staged absolute image
    assert(!s._absoluteGray && s._needsInitialFull && bus.writes.empty());
    s.beginGrayscale(bus, white.data(), GrayscaleMode::Absolute, RefreshMode::Half, false);
    s.copyGrayscaleLsb(bus, imageLsb.data());
    s.copyGrayscaleMsb(bus, imageMsb.data());
    std::fill(imageMsb.begin(), imageMsb.end(), 0);  // caller reuses the upload buffer
    s.displayGray(bus, imageMsb.data(), true, nullptr, true);
    std::vector<uint8_t> imageCommands;
    for (const auto& w : bus.writes) {
      imageCommands.push_back(w.command);
      if (w.command == 0x10) assert(w.bytes == Bytes(96000, 0x1B));
      if (w.command == 0x20) assert(w.bytes == Bytes(lut_picco_se0400_image, lut_picco_se0400_image + 535));
    }
    assert((imageCommands == std::vector<uint8_t>{0x04, 0x10, 0x20, 0x12, 0x02}));
    assert(s._needsInitialFull && s._needsPowerOn);
    // Charge strip over the retained image: B/W rectangle, then restore.
    s.requestResync(1);  // facade finishes an absolute session this way
    {
      Bytes strip(48000, 0xF0);
      for (const bool restore : {false, true}) {
        bus.clear();
        assert(s.displayRetainedGrayscaleOverlay(bus, restore ? nullptr : strip.data(), 8, 2, 16, 3));
        std::vector<uint8_t> commands;
        for (const auto& w : bus.writes) {
          commands.push_back(w.command);
          if (w.command == 0x20) assert(w.bytes == Bytes(lut_picco_se0400_image, lut_picco_se0400_image + 535));
          if (w.command != 0x10) continue;
          assert(w.bytes.size() == 96000);
          for (size_t i = 0; i < 48000; ++i) {
            const bool inside = !restore && i / 100 >= 2 && i / 100 < 5 && i % 100 >= 1 && i % 100 < 3;
            assert(w.bytes[2 * i] == (inside ? 0xFF : 0x1B) && w.bytes[2 * i + 1] == (inside ? 0x00 : 0x1B));
          }
        }
        assert((commands == std::vector<uint8_t>{0x04, 0x10, 0x20, 0x12, 0x02}));
      }
      bus.clear();
      assert(!s.displayRetainedGrayscaleOverlay(bus, strip.data(), 799, 0, 2, 1) && bus.writes.empty());
    }
    // Each group of four exercises B->B, B->W, W->B, W->W respectively.
    Bytes prior(48000, 0x33), target(48000, 0x55);
    s.display(bus, prior.data(), nullptr, RefreshMode::Full, false);
    bus.clear();
    s.beginGrayscale(bus, target.data(), GrayscaleMode::Overlay, RefreshMode::Fast, false);
    unsigned baseRefreshes = 0;
    lut = nullptr;
    data = nullptr;
    for (const auto& w : bus.writes) {
      if (w.command == 0x10) data = &w.bytes;
      if (w.command == 0x12) ++baseRefreshes;
      if (w.command == 0x20) lut = &w.bytes;
    }
    assert(baseRefreshes == 1 && lut && *lut == Bytes(lut_picco_se0400_fast, lut_picco_se0400_fast + 535));
    assert(data && *data == Bytes(96000, 0x9B));  // B->B top-up, B->W, W->B, W->W
    assert(memcmp(s._old, target.data(), target.size()) == 0);
    Bytes lsb(48000, 0x00), msb(48000, 0x00);
    lsb[0] = 0xF0;  // px 0-3 LSB
    msb[0] = 0xCC;  // px 0,1,4,5 MSB
    bus.clear();
    s.copyGrayscaleLsb(bus, lsb.data());
    s.copyGrayscaleMsb(bus, msb.data());
    const Bytes* g = nullptr;
    for (const auto& w : bus.writes)
      if (w.command == 0x10) g = &w.bytes;
    // px: both masks and LSB-only go out as 01, MSB-only as 10: 01 01 01 01 | 10 10 00 00
    assert(g && g->size() == 96000 && (*g)[0] == 0x55 && (*g)[1] == 0xA0);
    s.displayGray(bus, newFb.data(), false, nullptr, false);
    for (const auto& w : bus.writes)
      if (w.command == 0x20) lut = &w.bytes;
    assert(*lut == Bytes(lut_picco_se0400_gray, lut_picco_se0400_gray + 535));
    assert(lastRegister(bus, 0x02) == 0 && s._needsPowerOn);
    const auto old = Bytes(s._old, s._old + 48000);
    bus.clear();
    fakeDelayMs = 0;
    s.powerOffBetweenFrames(bus);
    assert(bus.writes.empty() && s._needsPowerOn && !s._needsInitialFull);
    assert(fakeDelayMs == 0 && old == Bytes(s._old, s._old + 48000));
    bus.clear();
    s.powerOffBetweenFrames(bus);
    assert(bus.writes.empty());
    s.display(bus, white.data(), nullptr, RefreshMode::Fast, false);
    bool poweredOn = false;
    for (const auto& w : bus.writes) {
      if (w.command == 0x04) poweredOn = true;
      if (w.command == 0x12) assert(poweredOn);
      assert(w.command != 0x07);  // no deep sleep
    }
    assert(poweredOn && s._needsPowerOn);  // whole-screen redraw powers back off
    expectSsd2677NightMode(s);
    Bytes grayBase(48000, 0x55), grayLsb(48000, 0x11), grayMsb(48000, 0x33);
    s.display(bus, grayBase.data(), nullptr, RefreshMode::Full, false);
    s.beginGrayscale(bus, grayBase.data(), GrayscaleMode::Overlay, RefreshMode::Fast, false);
    s.copyGrayscaleLsb(bus, grayLsb.data());
    s.copyGrayscaleMsb(bus, grayMsb.data());
    s.displayGray(bus, grayMsb.data(), false, nullptr, false);
    bus.clear();
    s.display(bus, grayBase.data(), nullptr, RefreshMode::Fast, false);
    unsigned activations = 0;
    for (const auto& w : bus.writes) {
      if (w.command == 0x10) assert(w.bytes == Bytes(96000, 0xB9));
      activations += w.command == 0x12;
    }
    assert(activations == 1 && !s._grayPending);
    s.deepSleep(bus);
    assert(lastRegister(bus, 0x07) == 0xA5);
    Ssd2677Driver noHistory;
    fakeHeapCapsFail = true;
    noHistory.begin(bus);
    fakeHeapCapsFail = false;
    noHistory.display(bus, white.data(), nullptr, RefreshMode::Full, false);
    bus.clear();
    noHistory.display(bus, black.data(), nullptr, RefreshMode::Fast, false);
    for (const auto& w : bus.writes)
      if (w.command == 0x20) assert(w.bytes == Bytes(lut_picco_se0400_full, lut_picco_se0400_full + 535));
    expectNightGray2677(s);
    expectDarkGray2677(s);
    expectDeferredRefresh2677(s);
    expectGray16Image(s);
    expectGray8Canvas(s);
    std::printf("Picco se0400: probe, init, packing, refresh and its deferral, 16-level image passed\n");
    return 0;
  }
  checkController("SSD1677");

  auto& d = static_cast<Ssd1677Driver&>(ssd1677Driver());
  // Sleep latches the display pins; the probe must release them before driving RESET.
  assert(fakeHoldReleased[BoardConfig::ACTIVE.display.rst]);
  // The probe issued Read User ID; an unrecognized ID goes on to CMD 0x70.
  assert(fakePanelCommand == (strcmp(which, "unknown") ? 0x2E : 0x70));
  const uint8_t variant = BoardConfig::ACTIVE.displayControllerVariant;
  const char* panelName = FreeInkDisplay(1, 2, 3, 4, 5, 6).getPanelName();
  const char* wantPanel = !strcmp(which, "depg")      ? "DEPG0397"
                          : !strcmp(which, "opm")     ? "OPM040B3"
                          : !strcmp(which, "unknown") ? nullptr
                                                      : "YRD0426";
  assert(wantPanel ? panelName && !strcmp(panelName, wantPanel) : panelName == nullptr);
  EpdBus bus;
  d.begin(bus);
  const Bytes gateOutput = (variant == PiccoYrd0426 || variant == PiccoDepg0397)
                               ? Bytes{0x01, 0x8F, 0x02} : Bytes{0xDF, 0x01, 0x02};
  bool sawGateOutput = false;
  for (const auto& write : bus.writes) {
    if (write.command == 0x01) {
      assert(write.bytes == gateOutput);
      sawGateOutput = true;
    }
  }
  assert(sawGateOutput);
  if (variant == PiccoDepg0397) {
    const std::vector<uint8_t> commands{0x12, 0x01, 0x11, 0x44, 0x45, 0x4E, 0x4F, 0x3C, 0x21, 0x0C, 0x18};
    const std::vector<Bytes> data{{}, {1, 0x8F, 2}, {1}, {0, 0, 3, 0x1F}, {1, 0x8F, 0, 0},
                                 {0, 0}, {1, 0x8F}, {1}, {0, 0}, {0xAE, 0xC7, 0xC3, 0xC0, 0x80}, {0x80}};
    assert(bus.writes.size() == commands.size());
    for (unsigned i = 0; i < commands.size(); ++i) {
      assert(bus.writes[i].command == commands[i]);
      assert(bus.writes[i].bytes == data[i]);
    }
  }
  if (argc > 2 && !strcmp(argv[2], "textaa")) {
    expectOnePassTextAa(d, variant == PiccoDepg0397);
    std::printf("Picco %s: one-pass text AA passed\n", which);
    return 0;
  }
  if (argc > 2 && !strcmp(argv[2], "retained")) {
    expectRetainedGrayStrip(d);
    std::printf("Picco %s: retained grayscale strip passed\n", which);
    return 0;
  }
  // First paint is the absolute FULL; then exercise each mode.
  if (!strcmp(which, "yrd") || !strcmp(which, "opm")) {
    assert(variant == (!strcmp(which, "yrd") ? 1 : 3));
    assert(lastRegister(bus, 0x3C) == (!strcmp(which, "yrd") ? 0x01 : 0x80));
    expectRefresh(d, RefreshMode::Full, 0xF4, nullptr);
    expectRefresh(d, RefreshMode::Half, 0x14, nullptr);
    expectRefresh(d, RefreshMode::Fast, 0x0C, lut_picco_fast);
    expectRefresh(d, RefreshMode::Fast, 0x0C, lut_picco_fast);  // rails remain on between UI paints
    d.setReaderRefresh(true);
    expectRefresh(d, RefreshMode::Fast, 0x1C, nullptr);
    d.setReaderRefresh(false);
    // First UI frame retains the reader waveform to erase reading-depth ink.
    expectRefresh(d, RefreshMode::Fast, 0x1C, nullptr);
    expectRefresh(d, RefreshMode::Fast, 0x0C, lut_picco_fast);
    expectRefresh(d, RefreshMode::Fast, 0x0C, lut_picco_fast);
    expectStockPiccoGray(d);
    expectRetainedGrayStrip(d);
  } else if (!strcmp(which, "depg")) {
    assert(variant == 2);
    // UI and reader share the custom pulse timing and voltage calibration.
    assert(d._cfg.fastLut == lut_picco_depg_custom_fast);
    assert(d._cfg.fastLut[10] == 0x88 && d._cfg.fastLut[20] == 0x44);
    assert(d._cfg.fastLut[50] == 8 && d._cfg.fastLut[51] == 1);
    expectRefresh(d, RefreshMode::Full, 0xE4, lut_picco_depg_full);
    expectRefresh(d, RefreshMode::Half, 0x04, lut_picco_depg_full);
    expectRefresh(d, RefreshMode::Fast, 0x0C, lut_picco_depg_custom_fast);
    d.setReaderRefresh(true);
    expectRefresh(d, RefreshMode::Fast, 0x0C, lut_picco_depg_custom_fast);
    expectRefresh(d, RefreshMode::Fast, 0x0C, lut_picco_depg_custom_fast);
    expectRefresh(d, RefreshMode::Fast, 0x0F, lut_picco_depg_custom_fast, true);
    expectRefresh(d, RefreshMode::Fast, 0xCC, lut_picco_depg_custom_fast);
    expectRefresh(d, RefreshMode::Fast, 0x0C, lut_picco_depg_custom_fast);
    d.setReaderRefresh(false);
    expectRefresh(d, RefreshMode::Fast, 0x0C, lut_picco_depg_custom_fast);
    expectRefresh(d, RefreshMode::Fast, 0x0C, lut_picco_depg_custom_fast);
    expectRefresh(d, RefreshMode::Fast, 0x0C, lut_picco_depg_custom_fast);
    expectStockPiccoGray(d);
    expectRetainedGrayStrip(d);
  } else {
    assert(variant == 0);
    assert(d._cfg.fullLut == nullptr && d._cfg.fastLut == nullptr && d._cfg.grayLut == lut_grayscale_sticky);
  }
  if (variant == PiccoDepg0397) { expectCustomDepg(d); expectStickyAaPower(d); }
  expectNightGray(d);
  expectIdlePower(d);
  std::printf("Picco %s: probe, config and refresh sequences passed\n", which);
  return 0;
}
