#include <cassert>
#include <algorithm>
#include <vector>
#include <cstdio>
#include <cstring>
#include <type_traits>
// Inspect private state without adding test-only methods to the SDK API.
#define private public
#include "FreeInkDisplay.h"
#include "src/driver/Ssd1677Driver.h"
#include "src/lut/Ssd1677Luts.h"
#include "src/driver/Uc8179Driver.h"
#include "src/driver/Uc8279X4Driver.h"
#undef private
#include "src/lut/X4ProTextAaLuts.h"

using namespace freeink;
using Bytes = std::vector<uint8_t>;

static uint8_t lastRegister(const EpdBus& bus, uint8_t command) {
  for (auto i = bus.writes.rbegin(); i != bus.writes.rend(); ++i)
    if (i->command == command && !i->bytes.empty()) return i->bytes[0];
  assert(false);
  return 0;
}

static const Bytes& lastPlane(const EpdBus& bus, uint8_t command) {
  for (auto i = bus.writes.rbegin(); i != bus.writes.rend(); ++i)
    if (i->command == command) return i->bytes;
  assert(false);
  static const Bytes empty;
  return empty;
}

static Bytes frame(unsigned seed) {
  Bytes b(48000);
  for (size_t i=0; i<b.size(); ++i) b[i]=uint8_t((i*37 + i/100*11 + seed) ^ (i>>8));
  return b;
}

template<class Driver>
static void testStream(bool reverse, unsigned gateOffset) {
  Driver d;
  EpdBus bus;
  const auto a=frame(3), b=frame(89);
  for (bool invert : {false,true}) {
    bus.clear();
    d.streamPlane(bus,0x13,a.data(),invert);
    assert(bus.writes.size()==1);
    const auto& w=bus.writes.front();
    assert(w.transactions==1 && w.bytes.size()==60000);
    for (unsigned y=0; y<600; ++y) for (unsigned x=0; x<100; ++x) {
      uint8_t expected=0xff;
      if (y>=gateOffset && y<gateOffset+480) {
        unsigned row=y-gateOffset;
        if (reverse) row=479-row;
        expected=a[row*100+x];
        if (invert) expected=uint8_t(~expected);
      }
      assert(w.bytes[y*100+x]==expected);
    }
  }
  bus.clear();
  d.streamPlaneXor(bus,0x13,a.data(),b.data());
  assert(bus.writes.size()==1 && bus.writes[0].transactions==1);
  assert(bus.writes[0].bytes.size()==60000);
  for (unsigned y=0; y<600; ++y) for (unsigned x=0; x<100; ++x) {
    uint8_t expected=0xff;
    if (y>=gateOffset && y<gateOffset+480) {
      unsigned row=y-gateOffset;
      if (reverse) row=479-row;
      expected=a[row*100+x]^b[row*100+x];
    }
    assert(bus.writes[0].bytes[y*100+x]==expected);
  }
  d._grayRefreshedOnce=true;
  d._oldPlaneValid=true;
  d._needFullClear=false;
  bus.clear();
  d.displayGrayscaleBase(bus,a.data(),RefreshMode::Full,false);
  assert(lastRegister(bus,0xe5)==0x1e); // honor Full even after an AA page
  for (const auto& w : bus.writes)
    if (w.command==0x10 || w.command==0x13) assert(w.transactions==1);
}

static void testSsd() {
  const auto b=frame(33);
  for (auto mode : {RefreshMode::Full,RefreshMode::Half,RefreshMode::Fast}) {
    EpdBus bus;
    Ssd1677Driver d;
    d.begin(bus);
    bus.clear();
    d.display(bus,b.data(),nullptr,mode,false);
    assert(lastRegister(bus,0x22)==(mode==RefreshMode::Full ? 0xf7 : 0xd7));
    bus.clear();
    d.display(bus,b.data(),nullptr,RefreshMode::Fast,false);
    assert(lastRegister(bus,0x22)==0xfc);
  }
  // The first-paint-after-boot/wake promotion must survive turnOff. A caller that powers
  // the panel down on every refresh (CrossPoint's sunlight fading fix) otherwise never
  // consumes the one-shot, and its differential FAST cannot clear the sleep screen.
  {
    EpdBus bus;
    Ssd1677Driver d;
    d.begin(bus);
    // The activation value is not the LAST 0x22 write here: 0xfc does not self-power-off,
    // so the driver follows it with the separate 0x22=0x03 power-down sequence.
    const auto activated = [&bus](uint8_t seq) {
      for (const auto& w : bus.writes)
        if (w.command == 0x22 && !w.bytes.empty() && w.bytes[0] == seq) return true;
      return false;
    };
    bus.clear();
    d.display(bus,b.data(),nullptr,RefreshMode::Fast,true);
    assert(activated(0xd7) && !activated(0xfc));
    bus.clear();
    d.display(bus,b.data(),nullptr,RefreshMode::Fast,true);
    assert(activated(0xfc) && !activated(0xd7));
  }
  EpdBus bus;
  Ssd1677Driver d;
  d.begin(bus);
  bus.clear();
  d.displayGray(bus,b.data(),false,nullptr,false);
  assert(lastRegister(bus,0x22)==0xcc && d._isScreenOn);
  d.deepSleep(bus);
  assert(lastRegister(bus,0x22)==3 && !d._isScreenOn);
  bus.clear();
  d.displayGray(bus,b.data(),true,nullptr,false);
  assert(lastRegister(bus,0x22)==0xcf && !d._isScreenOn);
}

template<class Driver>
static void testAsyncFrame() {
  Driver driver;
  FreeInkDisplay display(12,11,13,18,14,6);
  display._driver=&driver;
  display.begin();
  const auto submitted=frame(27), redrawn=frame(94);
  std::memcpy(display.getFrameBuffer(),submitted.data(),submitted.size());
  display.displayBufferAsync(FreeInkDisplay::FAST_REFRESH);
  assert(display.isRefreshPending());
  std::memcpy(display.getFrameBuffer(),redrawn.data(),redrawn.size());
  display.completeDisplay();
  // Finish must sync the frame actually submitted, even after the caller draws.
  const auto& old=display._bus.writes.back();
  assert(old.command==0x10 && old.bytes.size()==60000);
  for (unsigned y=0; y<480; ++y) {
    unsigned dst;
    if constexpr (std::is_same<Driver,Uc8179Driver>::value) dst=479-y;
    else dst=120+y;
    assert(std::equal(submitted.begin()+y*100,submitted.begin()+(y+1)*100,old.bytes.begin()+dst*100));
  }
  assert(!display.isRefreshPending());
  // Shadow-free entry points require the live frame to survive until finish.
  std::memcpy(display.getFrameBuffer(),redrawn.data(),redrawn.size());
  display.triggerDisplay(FreeInkDisplay::FAST_REFRESH,false);
  display.completeDisplay();
  const auto& next=display._bus.writes.back();
  unsigned firstRow=std::is_same<Driver,Uc8179Driver>::value ? 479 : 0;
  unsigned offset=std::is_same<Driver,Uc8179Driver>::value ? 0 : 12000;
  assert(std::equal(redrawn.begin()+firstRow*100,redrawn.begin()+(firstRow+1)*100,next.bytes.begin()+offset));
#ifdef EINK_DISPLAY_SINGLE_BUFFER_MODE
  std::memcpy(display.getFrameBuffer(),submitted.data(),submitted.size());
  display.displayAsyncImpl(FreeInkDisplay::FAST_REFRESH,false,true);
  display.completeDisplay();
  const auto& noShadow=display._bus.writes.back();
  assert(std::equal(submitted.begin()+firstRow*100,submitted.begin()+(firstRow+1)*100,
                    noShadow.bytes.begin()+offset));
#endif
  display.releaseBuffers();
  free(driver._grayBase);
}

#ifndef EINK_DISPLAY_SINGLE_BUFFER_MODE
// cleanupGrayscaleWithPreviousBuffer() only ever runs at the tail (or abort point) of a
// grayscale plane pass, and those passes use the write framebuffer as plane scratch. So
// frameBufferActive is the only thing that can be the displayed B/W frame: with the secondary
// away the driver must be told there is no baseline, never handed the plane. Handing it over
// rebases the controller's previous-frame RAM from the page's own glyph plane.
static void testGrayCleanupBaseline() {
  const auto page = frame(51), planeScratch = frame(77);

  // Secondary resident: the displayed frame is the baseline, and the write buffer is restored
  // from it so the next B/W draw starts from a real frame rather than plane bytes.
  {
    Uc8279X4Driver driver;
    FreeInkDisplay display(12, 11, 13, 18, 14, 6);
    display._driver = &driver;
    display.begin();
    std::memcpy(display.getFrameBuffer(), page.data(), page.size());
    display.displayBuffer(FreeInkDisplay::FAST_REFRESH);  // swaps: frameBufferActive == page
    std::memcpy(display.getFrameBuffer(), planeScratch.data(), planeScratch.size());
    display._bus.clear();
    display.cleanupGrayscaleWithPreviousBuffer();
    assert(std::count_if(display._bus.writes.begin(), display._bus.writes.end(),
                         [](const auto& w) { return w.command == 0x10; }) == 1);
    assert(driver._oldPlaneValid);
    assert(std::equal(page.begin(), page.end(), display.getFrameBuffer()));
    display.releaseBuffers();
    free(driver._grayBase);
  }

  // Secondary released, then lent: in both cases no copy of the displayed frame exists, so
  // nothing may be streamed into controller RAM and the driver must fall back to a clean sync.
  for (bool lend : {false, true}) {
    Uc8279X4Driver driver;
    FreeInkDisplay display(12, 11, 13, 18, 14, 6);
    display._driver = &driver;
    display.begin();
    std::memcpy(display.getFrameBuffer(), page.data(), page.size());
    display.displayBuffer(FreeInkDisplay::FAST_REFRESH);
    assert(driver._oldPlaneValid);
    if (lend) assert(display.borrowSecondaryBuffer(nullptr) != nullptr);
    else assert(display.releaseSecondaryBuffer());
    std::memcpy(display.getFrameBuffer(), planeScratch.data(), planeScratch.size());
    display._bus.clear();
    display.cleanupGrayscaleWithPreviousBuffer();
    for (const auto& w : display._bus.writes) assert(w.command != 0x10 && w.command != 0x13);
    assert(driver._needFullClear && !driver._oldPlaneValid);
    if (lend) assert(display.returnSecondaryBuffer());
    display.releaseBuffers();
    free(driver._grayBase);
  }
}
#endif

#ifndef EINK_DISPLAY_SINGLE_BUFFER_MODE
// The composed one-pass (the X3): the frame on screen becomes the old plane, white before and after,
// the host draws the page's gray into it, and the page becomes the frame on screen. Only a frame a
// display just put there may be used: after a lend nothing is known until the next display.
class ComposingDriver : public Uc8279X4Driver {
 public:
  bool composesOnePassText() const override { return true; }
  bool displayOnePassStart(EpdBus&, const uint8_t* oldPlane, const uint8_t* page, RefreshMode, bool) override {
    old.assign(oldPlane, oldPlane + 48000);
    shown.assign(page, page + 48000);
    ++frames;
    return false;
  }
  Bytes old, shown;
  unsigned frames = 0;
};

// The X4's composed one-pass: page in BW, the old plane in RED, the exchanged X4 Pro table, one 0xCC
// activation, then both planes become the page. A HALF keeps the B/W path.
// A lab's frame (development builds): both planes and the LUT exactly as given, one 0xCC activation, and the
// next display starts clean.
static void testSsdLabFrame() {
  Ssd1677Driver d;
  EpdBus bus;
  d.begin(bus);
  Bytes page(48000, 0xF0), red(48000, 0x3C), lut(110, 0x00);
  lut[0] = 0x15;
  lut[1] = 0x54;
  d.display(bus, page.data(), nullptr, RefreshMode::Full, false);
  bus.clear();
  assert(d.displayLabFrame(bus, page.data(), red.data(), lut.data()));
  assert(lastPlane(bus, 0x24) == page && lastPlane(bus, 0x26) == red);
  const Bytes& sent = lastPlane(bus, 0x32);
  assert(sent.size() == 105 && sent[0] == 0x15 && sent[1] == 0x54);
  assert(lastRegister(bus, 0x22) == 0xCC);
}

static void testSsdComposedOnePass() {
  const auto board = BoardConfig::ACTIVE.board;
  BoardConfig::ACTIVE.board = BoardConfig::Board::XteinkX4;
  Ssd1677Driver d;
  EpdBus bus;
  d.begin(bus);
  Bytes page(48000, 0xF0), mask(48000, 0x3C);
  d.display(bus, page.data(), nullptr, RefreshMode::Full, false);
  assert(d.supportsOnePassText() && d.composesOnePassText());
  bus.clear();
  assert(d.displayOnePassStart(bus, mask.data(), page.data(), RefreshMode::Fast, false));
  assert(lastPlane(bus, 0x24) == page && lastPlane(bus, 0x26) == mask);
  const Bytes& lut = lastPlane(bus, 0x32);
  assert(lut.size() == 105 && lut[0] == 0x15 && lut[10] == 0x0A && lut[11] == 0xAA && lut[30] == 0x00 && lut[31] == 0x80);
  assert(lastRegister(bus, 0x22) == 0xCC);
  d.displayFinish(bus, page.data());
  assert(lastPlane(bus, 0x24) == page && lastPlane(bus, 0x26) == page);
  bus.clear();
  d.displayOnePassStart(bus, mask.data(), page.data(), RefreshMode::Half, false);
  d.displayFinish(bus, page.data());
  for (const auto& w : bus.writes) assert(w.command != 0x32);  // no one-pass table on a clean refresh
  BoardConfig::ACTIVE.board = BoardConfig::Board::XteinkX4Pro;
  Ssd1677Driver pro;
  assert(!pro.supportsOnePassText());
  BoardConfig::ACTIVE.board = board;
}

static void testComposedOnePass() {
  ComposingDriver driver;
  FreeInkDisplay display(12, 11, 13, 18, 14, 6);
  display._driver = &driver;
  display.begin();
  assert(display.composesOnePassText());
  assert(display.beginOnePassMask() == nullptr);  // nothing of ours on the glass yet
  Bytes before(48000, 0xFF), page(48000, 0xFF);
  before[0] = 0x0F;  // left half black on screen
  page[0] = 0xF0;    // right half black on the new page
  page[1] = 0x00;
  std::memcpy(display.getFrameBuffer(), before.data(), before.size());
  display.displayBuffer(FreeInkDisplay::FAST_REFRESH);
  std::memcpy(display.getFrameBuffer(), page.data(), page.size());
  uint8_t* mask = display.beginOnePassMask();
  assert(mask && mask == display.getFrameBuffer());
  assert(mask[0] == 0x00 && mask[1] == 0x00 && mask[2] == 0xFF);  // only white before and after rests
  mask[1] = 0x01;                                                  // the host's gray
  display.displayOnePassFrame(FreeInkDisplay::FAST_REFRESH);
  assert(driver.frames == 1 && driver.shown == page);
  assert(driver.old[0] == 0x00 && driver.old[1] == 0x01 && driver.old[2] == 0xFF);
  assert(std::equal(page.begin(), page.end(), display.getFrameBuffer()));     // the write buffer keeps the page
  assert(std::equal(page.begin(), page.end(), display.frameBufferActive));    // and it is the frame on screen
  assert(display.beginOnePassMask() != nullptr);                              // so the next page may use it
  display.displayOnePassFrame(FreeInkDisplay::FAST_REFRESH);
  assert(driver.frames == 2);

  assert(display.borrowSecondaryBuffer(nullptr) != nullptr);
  assert(display.beginOnePassMask() == nullptr);
  assert(display.returnSecondaryBuffer());
  assert(display.beginOnePassMask() == nullptr);  // the borrower wrote over it
  display.displayBuffer(FreeInkDisplay::FAST_REFRESH);
  assert(display.beginOnePassMask() != nullptr);
  display.displayOnePassFrame(FreeInkDisplay::FAST_REFRESH);
  assert(driver.frames == 3);
  display.releaseBuffers();
  free(driver._grayBase);
}
#endif

// A driver with no grayscale implementation must never advertise support.
class BwOnlyDriver : public PanelDriver {
 public:
  uint32_t spiHz() const override { return 10000000; }
  BusyPolarity busyPolarity() const override { return BusyPolarity::ActiveHigh; }
  PanelGeometry geometry() const override { return {800, 480, 100, 48000}; }
  void begin(EpdBus&) override {}
  void deepSleep(EpdBus&) override {}
  void display(EpdBus&, const uint8_t*, const uint8_t*, RefreshMode, bool) override {}
};

class CombinedGrayDriver : public BwOnlyDriver {
 public:
  GrayscaleCapabilities grayscaleCapabilities(GrayscaleMode mode = GrayscaleMode::Overlay) const override {
    if (mode != GrayscaleMode::Overlay) return {};
    return {GrayscaleEncoding::OverlayMasks, GrayscaleBase::Combined, true, false, true};
  }
};

static void testCapabilities() {
  FreeInkDisplay display(1, 2, 3, 4, 5, 6);
  assert(!display.grayscaleCapabilities().supported());
  BwOnlyDriver bw;
  display._driver = &bw;
  assert(!display.grayscaleCapabilities().supported());
  assert(!display.supportsStripGrayscale());

  Ssd1677Driver ssd;
  display._driver = &ssd;
  auto caps = display.grayscaleCapabilities();
  assert(caps.supported() && caps.encoding == GrayscaleEncoding::OverlayMasks);
  assert(caps.stripUploads && caps.asyncBase && !caps.stagingWhileBusy);
  assert(caps.base == GrayscaleBase::Separate);
  assert(display.supportsStripGrayscale() == caps.stripUploads);
  assert(display.supportsAsyncGrayscaleBase() == caps.asyncBase);
  assert(display.grayscaleCapabilities(GrayscaleMode::Absolute).supported());
  assert(!display.grayscaleCapabilities(static_cast<GrayscaleMode>(255)).supported());
  display._inversionDirty = true;
  assert(!display.grayscaleCapabilities().asyncBase);
  assert(display.grayscaleCapabilities().stripUploads);
  assert(!display.supportsAsyncGrayscaleBase());
  display._inversionDirty = false;

  CombinedGrayDriver combined;
  display._driver = &combined;
  assert(display.combinesGrayscaleBase());
  assert(display.supportsBusyGrayscaleStaging());
  assert(!display.supportsAsyncGrayscaleBase());
  display._inverted = true;
  caps = display.grayscaleCapabilities();
  assert(!caps.supported() && !caps.stripUploads && !caps.asyncBase && !caps.stagingWhileBusy);
  assert(!display.combinesGrayscaleBase() && !display.supportsBusyGrayscaleStaging());
  assert(!display.supportsStripGrayscale());
  display._inverted = false;

  Uc8179Driver uc8179;
  Uc8279X4Driver uc8279;
  for (PanelDriver* driver : {static_cast<PanelDriver*>(&uc8179), static_cast<PanelDriver*>(&uc8279)}) {
    display._driver = driver;
    caps = display.grayscaleCapabilities();
    assert(caps.supported() && !caps.stripUploads && !caps.asyncBase && !caps.stagingWhileBusy);
    assert(display.grayscaleCapabilities(GrayscaleMode::Absolute).supported());
    assert(!display.grayscaleCapabilities(GrayscaleMode::Absolute).stripUploads);
  }
  // Queries must neither start a refresh nor write the panel bus.
  assert(display._bus.writes.empty() && !display.isRefreshPending());
}

static void testAbsolutePipeline() {
  Ssd1677Driver driver;
  FreeInkDisplay display(1, 2, 3, 4, 5, 6);
  display._driver = &driver;
  display.begin();
  const auto bw = frame(33), lsb = frame(14), msb = frame(29);
  std::memcpy(display.getFrameBuffer(), bw.data(), bw.size());
  const auto absolute = GrayscaleMode::Absolute;
  assert(display.grayscaleCapabilities(absolute).base == GrayscaleBase::Combined);
  for (bool strips : {false, true}) {
    display._bus.clear();
    assert(display.displayGrayscaleBase(absolute));
    assert(display._bus.writes.empty());
    if (strips) {
      for (unsigned y = 0; y < 480; y += 80) {
        display.writeGrayscalePlaneStrip(FreeInkDisplay::GRAY_PLANE_LSB, lsb.data() + y * 100, y, 80);
        display.writeGrayscalePlaneStrip(FreeInkDisplay::GRAY_PLANE_MSB, msb.data() + y * 100, y, 80);
      }
    } else display.copyGrayscaleBuffers(lsb.data(), msb.data());
    Bytes plane0, plane1;
    for (const auto& w : display._bus.writes) {
      if (w.command == 0x24) plane0.insert(plane0.end(), w.bytes.begin(), w.bytes.end());
      if (w.command == 0x26) plane1.insert(plane1.end(), w.bytes.begin(), w.bytes.end());
    }
    assert(plane0.size() == lsb.size() && plane1.size() == msb.size());
    for (size_t i = 0; i < lsb.size(); ++i) {
      assert(plane0[i] == uint8_t(~lsb[i]) && plane1[i] == uint8_t(~msb[i]));
    }
    for (const auto& w : display._bus.writes) assert(w.command != 0x20);
    display.displayGrayBuffer(false);
    assert(std::count_if(display._bus.writes.begin(), display._bus.writes.end(),
                         [](const auto& w) { return w.command == 0x20; }) == 1);
    assert(lastRegister(display._bus, 0x22) == 0xCC);
    assert(driver._isScreenOn && driver._needsGrayClear);
    display.cleanupGrayscaleBuffers(bw.data());
    assert(driver._needsGrayClear);
    display._bus.clear();
    display.displayBufferAsync(FreeInkDisplay::FAST_REFRESH);
    assert(lastRegister(display._bus, 0x22) == 0xD7);
    display.waitRefreshComplete();
    assert(!driver._needsGrayClear);
  }
  // Explicit shutdown must use its own activation after the gray refresh wait.
  assert(display.displayGrayscaleBase(absolute));
  display.copyGrayscaleBuffers(lsb.data(), msb.data());
  display._bus.clear();
  display.displayGrayBuffer(true);
  std::vector<uint8_t> activations;
  for (const auto& w : display._bus.writes) {
    if (w.command == 0x22) activations.push_back(w.bytes.at(0));
  }
  assert((activations == std::vector<uint8_t>{0xCC, 0x03}));
  assert(!driver._isScreenOn && driver._needsGrayClear);
  for (unsigned failure = 0; failure < 6; ++failure) {
    assert(display.displayGrayscaleBase(absolute));
    display._bus.clear();
    if (failure == 0) display.copyGrayscaleLsbBuffers(lsb.data()); // missing MSB
    if (failure == 1) display.writeGrayscalePlaneStrip(FreeInkDisplay::GRAY_PLANE_LSB, lsb.data(), 80, 80);
    if (failure == 2) display.writeGrayscalePlaneStrip(FreeInkDisplay::GRAY_PLANE_LSB, lsb.data(), 0, 481);
    if (failure == 3) display.copyGrayscaleLsbBuffers(nullptr);
    if (failure == 4) {
      display.copyGrayscaleBuffers(lsb.data(), msb.data());
      display.copyGrayscaleLsbBuffers(lsb.data()); // duplicate plane
    }
    if (failure == 5) {
      display.copyGrayscaleMsbBuffers(msb.data());
      display.copyGrayscaleLsbBuffers(lsb.data());
    }
    display.displayGrayBuffer();
    for (const auto& w : display._bus.writes) assert(w.command != 0x20);
    display.cleanupGrayscaleBuffers(nullptr);
    display._bus.clear();
    display.displayBuffer(FreeInkDisplay::FAST_REFRESH, true);
    assert(lastRegister(display._bus, 0x22) == 0xD7);
  }
  assert(display.displayGrayscaleBase(absolute));
  display.copyGrayscaleLsbBuffers(lsb.data());
  display.setInverted(true);
  assert(display._grayscaleMode == GrayscaleMode::Overlay);
  assert(!display.displayGrayscaleBase(absolute));
  display.setInverted(false);
  display._bus.clear();
  assert(display.displayGrayscaleBase(absolute));
  assert(display._bus.writes.empty() && display._inversionDirty);
  display.copyGrayscaleBuffers(lsb.data(), msb.data());
  display.displayGrayBuffer();
  assert(!display._inversionDirty);
  assert(display.displayGrayscaleBase(absolute));
  display.deepSleep();
  assert(display._grayscaleMode == GrayscaleMode::Overlay);
  display.releaseBuffers();

  Ssd1677Config unsupported = ssd1677DefaultConfig();
  unsupported.absoluteGrayscale = false;
  Ssd1677Driver other(unsupported);
  FreeInkDisplay noAbsolute(1, 2, 3, 4, 5, 6);
  noAbsolute._driver = &other;
  assert(!noAbsolute.displayGrayscaleBase(absolute));
  assert(noAbsolute._bus.writes.empty());
}

template<class Driver>
static void testUltraChipAbsolute(bool reverse, unsigned gateOffset, bool inverted) {
  for (bool snapshot : {false, true}) for (bool turnOff : {false, true}) {
    Driver driver;
    FreeInkDisplay display(1, 2, 3, 4, 5, 6);
    display._driver = &driver;
    display.begin();
    if (!snapshot) { free(driver._grayBase); driver._grayBase = nullptr; }
    const auto bw = frame(91), lsb = frame(13), msb = frame(29);
    memcpy(display.getFrameBuffer(), bw.data(), bw.size());
    assert(display.displayGrayscaleBase(GrayscaleMode::Absolute));
    display._bus.clear();
    display.copyGrayscaleBuffers(lsb.data(), msb.data());
    for (unsigned p = 0; p < 2; ++p) {
      const auto& input = p ? msb : lsb;
      const auto command = p ? 0x13 : 0x10;
      const auto it = std::find_if(display._bus.writes.begin(), display._bus.writes.end(),
                                  [command](const auto& w) { return w.command == command; });
      assert(it != display._bus.writes.end() && it->bytes.size() == 60000);
      for (unsigned y = 0; y < 600; ++y) for (unsigned x = 0; x < 100; ++x) {
        uint8_t expected = 0xff;
        if (y >= gateOffset && y < gateOffset + 480) {
          unsigned row = y - gateOffset;
          if (reverse) row = 479 - row;
          expected = input[row * 100 + x];
          if (inverted) expected = uint8_t(~expected);
        }
        assert(it->bytes[y * 100 + x] == expected);
      }
    }
    if (snapshot) assert(memcmp(driver._grayBase, bw.data(), bw.size()) == 0);
    display.displayGrayBuffer(turnOff);
    assert(driver._isScreenOn == !turnOff);
    assert(driver._needFullClear && !driver._absoluteInput);
    display.cleanupGrayscaleBuffers(bw.data());
    assert(driver._needFullClear);
    display.displayBuffer(FreeInkDisplay::FAST_REFRESH);
    assert(!driver._needFullClear);
    // Incomplete uploads are canceled without a gray activation and still force a clean.
    assert(display.displayGrayscaleBase(GrayscaleMode::Absolute));
    display.copyGrayscaleLsbBuffers(lsb.data());
    display._bus.clear();
    display.displayGrayBuffer();
    for (const auto& w : display._bus.writes) assert(w.command != 0x12);
    display.cleanupGrayscaleBuffers(bw.data());
    assert(driver._needFullClear && !driver._absoluteInput);
    display.displayGrayscaleBase(FreeInkDisplay::HALF_REFRESH);
    assert(!driver._absoluteInput);
    display.releaseBuffers();
    free(driver._grayBase);
    driver._grayBase = nullptr;
  }
}

static void testUc8179GrayShadeSplit() {
  Uc8179Driver driver;
  freeink::EpdBus bus;
  driver.begin(bus);
  const auto bw = frame(91), lsb = frame(13), msb = frame(29);
  for (auto mode : {GrayscaleMode::Absolute, GrayscaleMode::Overlay}) {
    driver.beginGrayscale(bus, bw.data(), mode, RefreshMode::Half, false);
    driver.copyGrayscaleLsb(bus, lsb.data());
    driver.copyGrayscaleMsb(bus, msb.data());
    bus.clear();
    driver.displayGray(bus, bw.data(), false, nullptr, mode == GrayscaleMode::Absolute);
    const auto light = std::find_if(bus.writes.begin(), bus.writes.end(),
                                    [](const auto& w) { return w.command == 0x22; });
    const auto dark = std::find_if(bus.writes.begin(), bus.writes.end(),
                                   [](const auto& w) { return w.command == 0x23; });
    assert(light != bus.writes.end() && dark != bus.writes.end());
    assert(light->bytes.size() == 42 && dark->bytes.size() == 42);
    assert(light->bytes[0] == 0x20 && light->bytes[1] == 2 && light->bytes[2] == 2);
    auto expected = light->bytes;
    expected[2] = 1;
    expected[3] = 2;
    assert(dark->bytes == expected);
    std::vector<size_t> pllWrites;
    size_t refresh = bus.writes.size();
    for (size_t i = 0; i < bus.writes.size(); ++i) {
      if (bus.writes[i].command == 0x30 && bus.writes[i].bytes.size() == 1) pllWrites.push_back(i);
      if (bus.writes[i].command == 0x12) refresh = i;
    }
    if (mode == GrayscaleMode::Absolute) {
      assert(pllWrites.size() == 2);
      assert(bus.writes[pllWrites[0]].bytes[0] == 0x05 && bus.writes[pllWrites[1]].bytes[0] == 0x06);
      assert(pllWrites[0] < refresh && refresh < pllWrites[1]);
    } else {
      assert(pllWrites.empty());
    }
  }
  free(driver._grayBase);
}

template<class Driver>
static void testDirectSleep() {
  Driver driver;
  FreeInkDisplay display(1, 2, 3, 4, 5, 6);
  display._driver = &driver;
  display.begin();
  const auto bw = frame(11), lsb = frame(17), msb = frame(29);
  std::memcpy(display.getFrameBuffer(), bw.data(), bw.size());
  const auto activations = [&]() {
    return std::count_if(display._bus.writes.begin(), display._bus.writes.end(),
                         [](const auto& w) { return w.command == 0x12; });
  };
  assert(display.grayscaleCapabilities(GrayscaleMode::Direct).base == GrayscaleBase::Combined);
  for (auto fallback : {FreeInkDisplay::HALF_REFRESH, FreeInkDisplay::FAST_REFRESH}) {
    display._bus.clear();
    assert(display.displayGrayscaleBase(GrayscaleMode::Direct, fallback));
    assert(activations() == 0);
    display.copyGrayscaleBuffers(lsb.data(), msb.data());
    assert(activations() == 0);
    display.displayGrayBuffer(false);
    assert(activations() == 1);
    display.cleanupGrayscaleBuffers(bw.data());
    display.displayBuffer(FreeInkDisplay::FAST_REFRESH, false);
    assert(activations() > 1);
  }
  display._bus.clear();
  assert(display.displayGrayscaleBase(GrayscaleMode::Direct));
  display.copyGrayscaleLsbBuffers(lsb.data());
  display.displayGrayBuffer(false);
  assert(activations() == 0);
  display.deepSleep();
  free(driver._grayBase);
}

static void testUc8279X4WaveformSelection() {
  Uc8279X4Driver driver;
  EpdBus bus;
  driver.begin(bus);
  const auto bw = frame(11);
  Bytes lsb(48000, 0), msb(48000, 0);
  const auto bank = [&]() {
    std::vector<Bytes> rows(5);
    for (const auto& w : bus.writes)
      if (w.command >= 0x20 && w.command <= 0x24 && w.bytes.size() == 49) rows[w.command - 0x20] = w.bytes;
    for (const auto& row : rows) assert(row.size() == 49);
    return rows;
  };
  const auto render = [&](GrayscaleMode mode, uint8_t mask, bool factory) {
    std::fill(msb.begin(), msb.end(), mask);
    driver.beginGrayscale(bus, bw.data(), mode, RefreshMode::Half, false);
    driver.copyGrayscaleLsb(bus, lsb.data());
    driver.copyGrayscaleMsb(bus, msb.data());
    bus.clear();
    driver.displayGray(bus, bw.data(), false, nullptr, factory);
    return bank();
  };
  const auto quality = render(GrayscaleMode::Direct, 0, false);
  assert(quality[2] != quality[3]);
  assert(render(GrayscaleMode::Absolute, 0, false) == quality);
  for (uint8_t variant : {0x02, 0x68, 0x69}) {
    BoardConfig::ACTIVE.displayControllerVariant = variant;
    for (uint8_t mask : {0x00, 0x01, 0x03}) {
      const auto text = render(GrayscaleMode::Overlay, mask, false);
      assert(text != quality);
      assert(text[2] == text[3]);
      const uint8_t timing = variant == 0x02 ? 2 : 3;
      for (unsigned t = 0; t < 5; ++t) {
        Bytes expected = {1, 2, timing, 1, 1, 1, 1, 0, 0, 0, 0, 0, 1, 1};
        expected.resize(49, 0);
        if (t == 1) expected[3] |= 0x40;
        if (t == 2 || t == 3) expected[2] |= 0x80;
        if (t == 4) expected[3] |= 0x80;
        assert(text[t] == expected);
      }
    }
    assert(render(GrayscaleMode::Overlay, 0x07, false) == quality);
    assert(render(GrayscaleMode::Overlay, 0, true) == quality);
    // An image pass must not leave the next sparse page on the quality bank.
    assert(render(GrayscaleMode::Overlay, 0x01, false) != quality);
  }
  BoardConfig::ACTIVE.displayControllerVariant = 0x68;
  free(driver._grayBase);
}

static void testStickyAbsolute() {
  BoardConfig::ACTIVE.board = BoardConfig::Board::Sticky;
  auto& driver = static_cast<Ssd1677Driver&>(ssd1677Driver());
  assert(driver._cfg.grayPowerUpFirst);
  assert(driver._cfg.fullSeqOverride == 0xF7);
  assert(driver.grayscaleCapabilities(GrayscaleMode::Absolute).supported());
  FreeInkDisplay display(1, 2, 3, 4, 5, 6);
  display._driver = &driver;
  display.begin();
  const auto bw = frame(33), lsb = frame(14), msb = frame(29);
  std::memcpy(display.getFrameBuffer(), bw.data(), bw.size());
  display._bus.clear();
  assert(display.displayGrayscaleBase(GrayscaleMode::Absolute));
  assert(display._bus.writes.empty());
  display.copyGrayscaleBuffers(lsb.data(), msb.data());
  display.displayGrayBuffer(false);
  assert(lastRegister(display._bus, 0x22) == 0xCC);
  assert(driver._isScreenOn);
  display.cleanupGrayscaleBuffers(bw.data());
  display._bus.clear();
  display.displayBuffer(FreeInkDisplay::FAST_REFRESH);
  assert(lastRegister(display._bus, 0x22) == 0xF7);
  display.releaseBuffers();
}

static void testMetalio() {
  BoardConfig::ACTIVE.board = BoardConfig::Board::MetalioEInk4;
  auto& driver = static_cast<Ssd1677Driver&>(ssd1677Driver());
  EpdBus bus;
  driver.begin(bus);
  assert(lastRegister(bus, 0x3C) == 0x01);
  assert(driver.grayscaleCapabilities(GrayscaleMode::Absolute).supported());
  assert(driver.grayscaleCapabilities().supported());
  const auto bw = frame(42);
  bus.clear();
  // Cold FAST uses the demo's two-partial black pulse, never OTP D7. Both
  // phases run the vendor B/W fast LUT (0xCC activation, no OTP 0xFC).
  driver.display(bus, bw.data(), nullptr, RefreshMode::Fast, false);
  assert(lastRegister(bus, 0x22) == 0xCC);
  unsigned activations = 0;
  std::vector<Bytes> oldPlanes;
  for (const auto& w : bus.writes) {
    if (w.command == 0x20) ++activations;
    if (w.command == 0x26) oldPlanes.push_back(w.bytes);
    if (w.command == 0x22) assert(w.bytes == Bytes({0xCC}));
    if (w.command == 0x32) assert(w.bytes == Bytes(lut_fast_metalio, lut_fast_metalio + 105));
    assert(w.command != 0x1A);
  }
  assert(activations == 2 && bus.waits == 2);
  assert(oldPlanes.size() == 3);
  assert(oldPlanes[0] == Bytes(bw.size(), 0xFF));
  assert(oldPlanes[1] == Bytes(bw.size(), 0x00));
  assert(oldPlanes[2] == bw);
  assert(!driver._customLutActive);
  bus.clear();
  driver.display(bus, bw.data(), nullptr, RefreshMode::Fast, false);
  assert(lastRegister(bus, 0x22) == 0xCC);
  assert(lastRegister(bus, 0x18) == 0x80);
  assert(lastRegister(bus, 0x3C) == 0x80);
  assert(lastRegister(bus, 0x2C) == 0x10);
  for (const auto& w : bus.writes) if (w.command == 0x21) assert(w.bytes == Bytes({0, 0}));
  assert(driver._isScreenOn);
  bus.clear();
  driver.displayGray(bus, bw.data(), false, nullptr, false);
  // X4 external-LUT AA path: LUT + voltage tail written, border parked at 0xC0,
  // absolute 0xCC activation.
  assert(lastRegister(bus, 0x22) == 0xCC);
  assert(lastRegister(bus, 0x3C) == 0xC0);
  bool lutWritten = false;
  for (const auto& w : bus.writes) if (w.command == 0x32) lutWritten = true;
  assert(lutWritten);
  // Exit grayscale the way the facade does so the next FAST isn't promoted.
  driver.cleanupGrayscaleBuffers(bus, bw.data());
  bus.clear();
  driver.displayStart(bus, bw.data(), bw.data(), RefreshMode::Fast, true);
  assert(lastRegister(bus, 0x22) == 0xCF);  // LUT paint, then analog/clock off
  driver.displayFinish(bus, bw.data());
  assert(lastRegister(bus, 0x22) == 0xCF);
  assert(!driver._isScreenOn);
  // Deferred FAST must resync both planes from the frame retained by the facade.
  assert(lastPlane(bus, 0x24) == bw && lastPlane(bus, 0x26) == bw);
  bus.clear();
  driver.displayStart(bus, bw.data(), nullptr, RefreshMode::Half, true);
  assert(bus.waits == 1);  // black pulse done; target waveform still pending
  unsigned pulses = 0;
  for (const auto& w : bus.writes) if (w.command == 0x20) ++pulses;
  assert(pulses == 2);
  assert(lastPlane(bus, 0x26) == Bytes(bw.size(), 0));
  driver.displayFinish(bus, bw.data());
  assert(lastPlane(bus, 0x26) == bw && lastPlane(bus, 0x24) == bw);
  assert(lastRegister(bus, 0x22) == 0xCF);
  driver.deepSleep(bus);
  assert(lastRegister(bus, 0x10) == 0x03);
}

// Waveform dose and charge tests ported from Free-Ink/freeink-sdk fix-display-updates (Justin Mitchell).
// finish() books each pixel's net drive, so its doses have to be the table's own.
static void testTextWaveformDoses() {
  auto polarity = [](unsigned level) { return level == 1 ? 1 : level == 2 ? -1 : 0; };
  const int ucDose[] = {-22, -1, -1, 20};
  for (unsigned row = 1; row <= 4; ++row) {
    int dose = 0;
    for (unsigned group = 0; group < 7; ++group)
      for (unsigned phase = 0; phase < 4; ++phase)
        dose += polarity((kX4ProTextUc8179[row * 42 + group * 6] >> (6 - phase * 2)) & 3) *
                kX4ProTextUc8179[row * 42 + group * 6 + phase + 1] *
                kX4ProTextUc8179[row * 42 + group * 6 + 5];
    assert(dose == ucDose[row - 1]);
  }
}

static void testTextAaCharge() {
  X4ProTextAa aa;
  aa.allocate(true);
  assert(aa.ready());
  Bytes base(48000, 0xff), zero(48000, 0);
  base[0] = 0x7f;
  auto compose = [&] {
    aa.begin(base.data());
    aa.stage(0, zero.data(), 0, 480);
    aa.stage(1, zero.data(), 0, 480);
    assert(aa.compose());
  };
  compose();
  assert(aa.high()[0] == 0x7f && aa.low()[0] == 0x7f);
  aa.finish(17, -2, -2, -24);
  assert(aa._median == -24);
  compose();
  assert(aa.high()[1] == 0xff && aa.low()[1] == 0);  // unchanged white holds
  aa.finish(17, -2, -2, -24);
  base[0] = 0xff;
  compose();
  assert(aa.low()[0] == 0x80);  // old black must receive the white drive
  aa.finish(17, -2, -2, -24);
  compose();
  assert(aa.low()[0] == 0x80);  // accumulated positive charge needs another pulse
  aa.finish(17, -2, -2, -24);
  for (int i = 0; i < 200; ++i) {
    compose();
    aa.finish(17, -2, -2, -24);
  }
  assert(aa.low()[0] == 0);  // white eventually settles back to hold
  aa.begin(base.data());
  aa.stage(0, zero.data(), 80, 80);
  assert(!aa.compose());
  aa.invalidate();
  assert(!aa.onPanel() && !aa._chargeValid);

  // Planes staged ahead of their frame outlive what a B/W refresh drops.
  aa.stage(0, zero.data(), 0, 480);
  aa.stage(1, zero.data(), 0, 480);
  aa.dropHistory();
  aa.beginStaged(base.data());
  assert(aa.compose());

  fakeHeapCapsFail = true;
  X4ProTextAa failed;
  failed.allocate(true);
  assert(!failed.ready());
  fakeHeapCapsFail = false;
}

static unsigned writesOf(const EpdBus& bus, uint8_t command) {
  unsigned count = 0;
  for (const auto& w : bus.writes) count += w.command == command;
  return count;
}

static void testOnePassFrame() {
  Uc8179Driver d;
  EpdBus bus;
  d.begin(bus);
  Bytes base(48000, 0xff), lsb(48000, 0), msb(48000, 0);
  base[0] = 0x7f;
  lsb[0] = 0x40;
  msb[0] = 0x20;
  const auto stage = [&] {
    d.copyGrayscaleLsb(bus, lsb.data());
    d.copyGrayscaleMsb(bus, msb.data());
  };
  d.display(bus, base.data(), nullptr, RefreshMode::Full, false);
  assert(d.supportsOnePassText());
  assert(!d.supportsGrayFrame() && !d._textAa.ready());  // no block until a host asks
  d.setReaderRefresh(true);
  assert(d.supportsGrayFrame());

  bus.clear();
  stage();
  assert(bus.writes.empty());  // staged for the frame, not uploaded
  assert(d.displayGrayFrameStart(bus, base.data(), RefreshMode::Fast, false));
  assert(writesOf(bus, 0x12) == 1 && bus.writes.back().command == 0x12);  // one waveform, left running
  assert(d._pendingRefresh && d._textAa.onPanel() && !d._textAa.active());
  assert(d._textAa.high()[0] == 0x1f && (d._textAa.low()[0] & 0xe0) == 0x60);  // black + both gray masks
  assert(lastPlane(bus, 0x10)[479 * 100] == d._textAa.low()[0]);
  assert(lastPlane(bus, 0x13)[479 * 100] == d._textAa.high()[0]);

  bus.clear();
  d.displayFinish(bus, base.data());
  assert(!d._pendingRefresh && writesOf(bus, 0x12) == 0);
  assert(lastPlane(bus, 0x10) == lastPlane(bus, 0x13));  // the next diff starts from the B/W frame
  assert(lastPlane(bus, 0x10)[479 * 100] == d._textAa.high()[0]);
  assert(d._oldPlaneValid && !d._needFullClear && d._redriveAfterGray);

  stage();
  d.displayGrayFrame(bus, base.data(), RefreshMode::Fast, false);
  assert(!d._pendingRefresh);
  assert(d._textAa.high()[1] == 0xff && d._textAa.low()[1] == 0);  // unchanged white holds

  // A B/W frame between staging and the page turn ends the history, not the planes.
  stage();
  d.display(bus, base.data(), nullptr, RefreshMode::Fast, false);
  assert(!d._textAa.onPanel());
  assert(d.displayGrayFrameStart(bus, base.data(), RefreshMode::Fast, false));
  d.displayFinish(bus, base.data());

  // A clean refresh goes first and keeps the planes too.
  stage();
  bus.clear();
  assert(d.displayGrayFrameStart(bus, base.data(), RefreshMode::Half, false));
  assert(writesOf(bus, 0x12) == 2);
  d.displayFinish(bus, base.data());

  // Planes that never completed still get the page up, in B/W.
  d.copyGrayscaleLsb(bus, lsb.data());
  bus.clear();
  assert(!d.displayGrayFrameStart(bus, base.data(), RefreshMode::Fast, false));
  assert(writesOf(bus, 0x12) == 1 && !d._pendingRefresh);

  // Outside a book page the planes go to the controller as they always did.
  d.setReaderRefresh(false);
  assert(!d.supportsGrayFrame());
  bus.clear();
  d.copyGrayscaleLsb(bus, lsb.data());
  assert(writesOf(bus, 0x10) == 1);

  fakeHeapCapsFail = true;
  Uc8179Driver noMemory;
  noMemory.begin(bus);
  noMemory.setReaderRefresh(true);
  assert(!noMemory.supportsGrayFrame());
  fakeHeapCapsFail = false;
}

#ifndef EINK_DISPLAY_SINGLE_BUFFER_MODE
// A deferred refresh whose finish re-reads the frame it painted (the X3's DTM1 sync): a host can take
// the other buffer while the panel paints, and the finish still gets the painted frame.
class DeferredDriver : public BwOnlyDriver {
 public:
  bool displayStart(EpdBus&, const uint8_t* fb, const uint8_t*, RefreshMode, bool) override {
    started.assign(fb, fb + 48000);
    return true;
  }
  void displayFinish(EpdBus&, const uint8_t* fb) override {
    finished.assign(fb, fb + 48000);
    ++finishes;
  }
  Bytes started, finished;
  int finishes = 0;
};

static void testLendDuringRefresh() {
  DeferredDriver driver;
  FreeInkDisplay display(12, 11, 13, 18, 14, 6);
  display._driver = &driver;
  display.begin();
  display._panelSel = FreeInkDisplay::PanelSel::X3;
  const auto popup = frame(5);
  std::memcpy(display.getFrameBuffer(), popup.data(), popup.size());
  display.triggerDisplay(FreeInkDisplay::FAST_REFRESH, false);
  assert(display.isRefreshPending());
  uint8_t* const shown = display.frameBufferActive;
  size_t size = 0;
  uint8_t* const lent = display.borrowSecondaryBufferDuringRefresh(&size);
  assert(lent != nullptr && lent != shown && size == 48000);
  assert(display.isRefreshPending() && driver.finishes == 0);  // nothing waited for the waveform
  assert(display.getFrameBuffer() == shown && !display.hasSecondaryBuffer());
  assert(display.borrowSecondaryBufferDuringRefresh(nullptr) == nullptr);  // one borrower at a time
  std::memset(lent, 0x55, size);  // the borrower writes its arena
  display.finishDisplayAsync();
  assert(driver.finishes == 1 && driver.finished == popup);  // the finish synced the painted frame
  assert(std::equal(popup.begin(), popup.end(), display.getFrameBuffer()));
  assert(display.returnSecondaryBuffer());
  assert(std::equal(popup.begin(), popup.end(), display.frameBufferActive));  // seeded from the live frame
  // Elsewhere it is borrowSecondaryBuffer(): the refresh is drained first.
  display._panelSel = FreeInkDisplay::PanelSel::X4;
  display.triggerDisplay(FreeInkDisplay::FAST_REFRESH, false);
  assert(display.isRefreshPending());
  assert(display.borrowSecondaryBufferDuringRefresh(nullptr) != nullptr);
  assert(!display.isRefreshPending() && driver.finishes == 2);
  assert(display.returnSecondaryBuffer());
  display.releaseBuffers();
}
#endif

int main(int argc, char** argv) {
  if (argc > 1 && std::strcmp(argv[1], "metalio") == 0) {
    testMetalio();
    std::puts("Metalio refresh, temperature, grayscale fallback and async power-down passed");
    return 0;
  }
  if (argc > 1) {
    testStickyAbsolute();
    std::puts("Sticky absolute capability, activation, power-down and B/W recovery passed");
    return 0;
  }
  testUc8179GrayShadeSplit();
  testTextWaveformDoses();
  testTextAaCharge();
  testOnePassFrame();
  testUc8279X4WaveformSelection();
  testDirectSleep<Uc8179Driver>();
  testDirectSleep<Uc8279X4Driver>();
  testUltraChipAbsolute<Uc8179Driver>(true, 0, false);
  for (uint8_t variant : {0x02, 0x68, 0x69}) {
    BoardConfig::ACTIVE.displayControllerVariant = variant;
    testUltraChipAbsolute<Uc8279X4Driver>(false, 120, true);
  }
  testAbsolutePipeline();
  testCapabilities();
  testStream<Uc8179Driver>(true,0);
  testStream<Uc8279X4Driver>(false,120);
  testSsd();
  testAsyncFrame<Uc8179Driver>();
  testAsyncFrame<Uc8279X4Driver>();
#ifndef EINK_DISPLAY_SINGLE_BUFFER_MODE
  testGrayCleanupBaseline();
  testComposedOnePass();
  testSsdComposedOnePass();
  testSsdLabFrame();
  testLendDuringRefresh();
#endif
  std::puts("Pro plane bytes, transaction counts, clean refreshes, power state and async frame ownership passed");
}
