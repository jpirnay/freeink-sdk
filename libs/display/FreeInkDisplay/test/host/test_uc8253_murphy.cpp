#include <BoardConfig.h>

#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>

#include "driver/Uc8253MurphyDriver.h"
#include "lut/Uc8253MurphyLuts.h"

using namespace freeink;

namespace {
constexpr uint16_t FB_W = 416, FB_H = 240, FB_WB = FB_W / 8, CTRL_W = 240, CTRL_H = 416, CTRL_WB = CTRL_W / 8;
constexpr size_t PLANE = static_cast<size_t>(FB_WB) * FB_H;

struct Bits {
  std::vector<uint8_t> bytes = std::vector<uint8_t>(PLANE, 0xFF);  // white
  void set(uint16_t x, uint16_t y, bool on) {
    uint8_t& byte = bytes[static_cast<size_t>(y) * FB_WB + (x >> 3)];
    const uint8_t mask = static_cast<uint8_t>(0x80 >> (x & 7));
    byte = on ? byte | mask : byte & ~mask;
  }
  const uint8_t* data() const { return bytes.data(); }
};

Bits blank(uint8_t fill) {
  Bits bits;
  bits.bytes.assign(PLANE, fill);
  return bits;
}

// Framebuffer pixel (x,y) sits at controller (239-y, x).
bool ramBit(const std::vector<uint8_t>& ram, uint16_t x, uint16_t y) {
  const uint16_t cx = static_cast<uint16_t>(FB_H - 1 - y);
  return (ram[static_cast<size_t>(x) * CTRL_WB + (cx >> 3)] >> (7 - (cx & 7))) & 1;
}

const EpdBus::Write* last(const EpdBus& bus, uint8_t command) {
  for (auto it = bus.writes.rbegin(); it != bus.writes.rend(); ++it) {
    if (it->command == command) return &*it;
  }
  return nullptr;
}

unsigned count(const EpdBus& bus, uint8_t command) {
  unsigned n = 0;
  for (const auto& write : bus.writes) n += write.command == command;
  return n;
}

int netDrive(const uint8_t* lut) {
  int net = 0;
  for (size_t group = 0; group < 42; group += 7) {
    for (size_t phase = 1; phase <= 4; phase++) {
      const uint8_t byte = lut[group + phase];
      if ((byte >> 6) == 1) net += byte & 0x3F;
      if ((byte >> 6) == 2) net -= byte & 0x3F;
    }
  }
  return net;
}

int totalFrames(const uint8_t* lut) {
  int frames = 0;
  for (size_t group = 0; group < 42; group += 7) {
    for (size_t phase = 1; phase <= 4; phase++) frames += lut[group + phase] & 0x3F;
  }
  return frames;
}

Uc8253MurphyConfig withGray() {
  Uc8253MurphyConfig cfg = uc8253MurphyDefaultConfig();
  cfg.gray = {MURPHY_LUT_20_GRAY, MURPHY_LUT_21_GRAY, MURPHY_LUT_22_GRAY, MURPHY_LUT_23_GRAY, MURPHY_LUT_24_GRAY};
  return cfg;
}

void nudge(Uc8253MurphyDriver& driver, EpdBus& bus, const Bits& lsb, const Bits& msb) {
  driver.copyGrayscaleLsb(bus, lsb.data());
  driver.copyGrayscaleMsb(bus, msb.data());
  driver.displayGray(bus, nullptr, false, nullptr, false);
}

void testFastGivesWhiteTheLongerDriveAndVcomCoversIt() {
  assert(netDrive(MURPHY_LUT_22_FAST) == -20);  // black to white
  assert(netDrive(MURPHY_LUT_23_FAST) == 15);   // white to black
  // The longest LUT sets the refresh, and the IC fills what is shorter with 0 V: VCOM has to span it.
  for (const uint8_t* lut : {MURPHY_LUT_21_FAST, MURPHY_LUT_22_FAST, MURPHY_LUT_23_FAST, MURPHY_LUT_24_FAST}) {
    assert(totalFrames(lut) <= totalFrames(MURPHY_LUT_20_FAST));
  }
}

void testTheHostOwnsTheCadence() {
  EpdBus bus;
  Uc8253MurphyDriver driver;
  driver.begin(bus);
  Bits page = blank(0xFF);
  for (int i = 0; i < 30; i++) {
    bus.clear();
    driver.display(bus, page.data(), page.data(), RefreshMode::Fast, false);
    assert(!std::memcmp(last(bus, 0x22)->bytes.data(), MURPHY_LUT_22_FAST, sizeof(MURPHY_LUT_22_FAST)));
  }
}

void testABoardCanStillAskTheDriverToPromote() {
  Uc8253MurphyConfig cfg = uc8253MurphyDefaultConfig();
  cfg.ghostClearInterval = 3;
  EpdBus bus;
  Uc8253MurphyDriver driver(cfg);
  driver.begin(bus);
  Bits page = blank(0xFF);
  for (int i = 0; i < 8; i++) {
    bus.clear();
    driver.display(bus, page.data(), page.data(), RefreshMode::Fast, false);
    const bool full = !std::memcmp(last(bus, 0x22)->bytes.data(), MURPHY_LUT_22_DEFAULT, sizeof(MURPHY_LUT_22_DEFAULT));
    assert(full == (i % 4 == 3));
  }
}

void testWithoutAGrayBankNothingChanges() {
  EpdBus bus;
  Uc8253MurphyDriver driver;
  assert(!driver.grayscaleCapabilities().supported());
  driver.begin(bus);
  bus.clear();
  Bits lsb, msb;
  nudge(driver, bus, lsb, msb);
  assert(bus.writes.empty());
}

void testGrayBankIsBalancedAndSelectsByPlane() {
  for (const uint8_t* lut : {MURPHY_LUT_21_GRAY, MURPHY_LUT_22_GRAY, MURPHY_LUT_23_GRAY, MURPHY_LUT_24_GRAY}) {
    assert(netDrive(lut) == 0);
  }
  const auto cfg = withGray();
  EpdBus bus;
  Uc8253MurphyDriver driver(cfg);
  assert(driver.grayscaleCapabilities().encoding == GrayscaleEncoding::OverlayMasks);
  assert(!driver.grayscaleCapabilities(GrayscaleMode::Absolute).supported());
  driver.begin(bus);
  bus.clear();

  Bits lsb = blank(0x00), msb = blank(0x00);
  msb.set(5, 7, true);  // light: LSB 0, MSB 1
  lsb.set(9, 7, true);  // dark: LSB 1, MSB 1
  msb.set(9, 7, true);
  nudge(driver, bus, lsb, msb);

  assert(count(bus, 0x12) == 1);
  const auto* dtm1 = last(bus, 0x10);
  const auto* dtm2 = last(bus, 0x13);
  assert(!ramBit(dtm1->bytes, 5, 7) && ramBit(dtm2->bytes, 5, 7));  // {NEW 1, OLD 0}: LUT 22
  assert(ramBit(dtm1->bytes, 9, 7) && ramBit(dtm2->bytes, 9, 7));   // {NEW 1, OLD 1}: LUT 21
  assert(!ramBit(dtm1->bytes, 20, 7) && !ramBit(dtm2->bytes, 20, 7));
  assert(!std::memcmp(last(bus, 0x22)->bytes.data(), MURPHY_LUT_22_GRAY, sizeof(MURPHY_LUT_22_GRAY)));
  assert(!std::memcmp(last(bus, 0x21)->bytes.data(), MURPHY_LUT_21_GRAY, sizeof(MURPHY_LUT_21_GRAY)));

  // LUTs before planes before the refresh: the nudge must not fire on stale RAM.
  size_t order[4] = {};
  for (size_t i = 0; i < bus.writes.size(); i++) {
    if (bus.writes[i].command == 0x24) order[0] = i;
    if (bus.writes[i].command == 0x10) order[1] = i;
    if (bus.writes[i].command == 0x13) order[2] = i;
    if (bus.writes[i].command == 0x12) order[3] = i;
  }
  assert(order[0] < order[1] && order[1] < order[2] && order[2] < order[3]);
}

void testFastDrivesGrayAwayAndOnlyGray() {
  const auto cfg = withGray();
  EpdBus bus;
  Uc8253MurphyDriver driver(cfg);
  driver.begin(bus);

  // Page A: black at the three test pixels, the first two of them gray after the nudge.
  Bits page = blank(0xFF);
  page.set(3, 3, false);
  page.set(4, 3, false);
  page.set(5, 3, false);
  Bits lsb = blank(0x00), msb = blank(0x00);
  msb.set(3, 3, true);
  msb.set(4, 3, true);
  nudge(driver, bus, lsb, msb);

  // Page B keeps 3 and 5 black and clears 4.
  Bits next = page;
  next.set(4, 3, true);
  bus.clear();
  driver.display(bus, next.data(), page.data(), RefreshMode::Fast, false);
  const auto* old = last(bus, 0x10);
  const auto* fresh = last(bus, 0x13);
  assert(ramBit(old->bytes, 3, 3) && !ramBit(fresh->bytes, 3, 3));   // gray to black: WK, full drive
  assert(!ramBit(old->bytes, 4, 3) && ramBit(fresh->bytes, 4, 3));   // gray to white: KW, as any change
  assert(!ramBit(old->bytes, 5, 3) && !ramBit(fresh->bytes, 5, 3));  // clean black stays: KK, rests
  assert(ramBit(old->bytes, 100, 100) && ramBit(fresh->bytes, 100, 100));  // clean white stays: WW

  // The mark lasts one refresh: the gray is gone now.
  Bits third = next;
  bus.clear();
  driver.display(bus, third.data(), next.data(), RefreshMode::Fast, false);
  assert(!ramBit(last(bus, 0x10)->bytes, 3, 3));
}

void testFastWithoutAPreviousFrameIsAFullRefresh() {
  EpdBus bus;
  Uc8253MurphyDriver driver;
  driver.begin(bus);
  Bits page = blank(0xFF);
  page.set(3, 3, false);
  bus.clear();
  driver.display(bus, page.data(), nullptr, RefreshMode::Fast, false);
  assert(!std::memcmp(last(bus, 0x22)->bytes.data(), MURPHY_LUT_22_DEFAULT, sizeof(MURPHY_LUT_22_DEFAULT)));
  assert(last(bus, 0x10)->bytes == last(bus, 0x13)->bytes);
  bus.clear();
  driver.display(bus, page.data(), page.data(), RefreshMode::Fast, false);
  assert(!std::memcmp(last(bus, 0x22)->bytes.data(), MURPHY_LUT_22_FAST, sizeof(MURPHY_LUT_22_FAST)));
}

void testGrayPixelsClearWithoutAPreviousFrame() {
  const auto cfg = withGray();
  EpdBus bus;
  Uc8253MurphyDriver driver(cfg);
  driver.begin(bus);
  Bits lsb = blank(0x00), msb = blank(0x00);
  msb.set(3, 3, true);
  nudge(driver, bus, lsb, msb);
  Bits next = blank(0xFF);
  bus.clear();
  driver.display(bus, next.data(), nullptr, RefreshMode::Fast, false);
  assert(!std::memcmp(last(bus, 0x22)->bytes.data(), MURPHY_LUT_22_DEFAULT, sizeof(MURPHY_LUT_22_DEFAULT)));
  assert(last(bus, 0x10)->bytes == last(bus, 0x13)->bytes);
  bus.clear();
  driver.display(bus, next.data(), next.data(), RefreshMode::Fast, false);
  assert(ramBit(last(bus, 0x10)->bytes, 3, 3));  // a full refresh leaves no gray for FAST to clear
}

void testASecondNudgeWouldLiftGrayFurtherSoItIsRefused() {
  const auto cfg = withGray();
  EpdBus bus;
  Uc8253MurphyDriver driver(cfg);
  driver.begin(bus);
  Bits lsb = blank(0x00), msb = blank(0x00);
  msb.set(3, 3, true);
  nudge(driver, bus, lsb, msb);
  bus.clear();
  nudge(driver, bus, lsb, msb);
  assert(count(bus, 0x12) == 0);

  Bits page = blank(0xFF);
  driver.display(bus, page.data(), page.data(), RefreshMode::Fast, false);
  bus.clear();
  nudge(driver, bus, lsb, msb);
  assert(count(bus, 0x12) == 1);
}

void testANudgeRightAfterAPageSkipsTheResetAndSwapsThePlanes() {
  const auto cfg = withGray();
  EpdBus bus;
  Uc8253MurphyDriver driver(cfg);
  driver.begin(bus);
  Bits page = blank(0xFF);
  driver.display(bus, page.data(), page.data(), RefreshMode::Fast, false);
  bus.clear();

  Bits lsb = blank(0x00), msb = blank(0x00);
  msb.set(5, 7, true);  // light: MSB only
  nudge(driver, bus, lsb, msb);

  assert(count(bus, 0x01) == 0 && count(bus, 0x04) == 0);  // no init, no power on
  assert(count(bus, 0x12) == 1);
  assert(ramBit(last(bus, 0x10)->bytes, 5, 7) && !ramBit(last(bus, 0x13)->bytes, 5, 7));
}

void testAPoweredOffControllerTakesTheFullPathWithStraightPlanes() {
  const auto cfg = withGray();
  EpdBus bus;
  Uc8253MurphyDriver driver(cfg);
  driver.begin(bus);
  Bits page = blank(0xFF);
  driver.display(bus, page.data(), page.data(), RefreshMode::Fast, true);  // the sunlight fix powers off
  bus.clear();

  Bits lsb = blank(0x00), msb = blank(0x00);
  msb.set(5, 7, true);
  nudge(driver, bus, lsb, msb);

  assert(count(bus, 0x01) == 1 && count(bus, 0x04) == 1);
  assert(!ramBit(last(bus, 0x10)->bytes, 5, 7) && ramBit(last(bus, 0x13)->bytes, 5, 7));
}

void testASleepEndsTheWarmPath() {
  const auto cfg = withGray();
  EpdBus bus;
  Uc8253MurphyDriver driver(cfg);
  driver.begin(bus);
  // Sleep resets the controller: a nudge after it must not assume the roles are traded.
  Bits page = blank(0xFF);
  driver.display(bus, page.data(), page.data(), RefreshMode::Fast, false);
  driver.deepSleep(bus);
  driver.begin(bus);
  bus.clear();
  Bits lsb = blank(0x00), msb = blank(0x00);
  msb.set(5, 7, true);
  nudge(driver, bus, lsb, msb);
  assert(count(bus, 0x01) == 1);
  assert(!ramBit(last(bus, 0x10)->bytes, 5, 7));
}

void testAnAbandonedPassShowsNothing() {
  const auto cfg = withGray();
  EpdBus bus;
  Uc8253MurphyDriver driver(cfg);
  driver.begin(bus);
  bus.clear();
  Bits lsb = blank(0x00);
  driver.copyGrayscaleLsb(bus, lsb.data());  // MSB never arrives
  driver.displayGray(bus, nullptr, false, nullptr, false);
  assert(bus.writes.empty());
  // Nor does a later page inherit it: nothing was left gray.
  Bits page = blank(0x00), next = blank(0x00);
  driver.display(bus, next.data(), page.data(), RefreshMode::Fast, false);
  assert(!ramBit(last(bus, 0x10)->bytes, 3, 3));
}

void testAFullRefreshClearsGrayWithoutMarks() {
  const auto cfg = withGray();
  EpdBus bus;
  Uc8253MurphyDriver driver(cfg);
  driver.begin(bus);
  Bits lsb = blank(0x00), msb = blank(0x00);
  msb.set(3, 3, true);
  nudge(driver, bus, lsb, msb);
  Bits page = blank(0xFF);
  bus.clear();
  driver.display(bus, page.data(), page.data(), RefreshMode::Full, false);
  assert(last(bus, 0x10)->bytes == last(bus, 0x13)->bytes);
  bus.clear();
  driver.display(bus, page.data(), page.data(), RefreshMode::Fast, false);
  assert(ramBit(last(bus, 0x10)->bytes, 3, 3));
}
}  // namespace

Uc8253MurphyConfig withText() {
  Uc8253MurphyConfig cfg = withGray();
  cfg.text = {MURPHY_LUT_20_TEXT, MURPHY_LUT_21_TEXT, MURPHY_LUT_22_TEXT, MURPHY_LUT_23_TEXT, MURPHY_LUT_24_TEXT};
  return cfg;
}

bool uniformPlane(const EpdBus& bus, uint8_t command, uint8_t fill) {
  const auto* write = last(bus, command);
  if (!write || write->bytes.size() != static_cast<size_t>(CTRL_WB) * CTRL_H) return false;
  for (uint8_t byte : write->bytes) {
    if (byte != fill) return false;
  }
  return true;
}

void testTextBankIsAsLongAsFastAndGrayNetsToZero() {
  for (const uint8_t* table : {MURPHY_LUT_20_TEXT, MURPHY_LUT_21_TEXT, MURPHY_LUT_22_TEXT}) assert(totalFrames(table) == 20);
  assert(totalFrames(MURPHY_LUT_23_TEXT) <= 20 && totalFrames(MURPHY_LUT_24_TEXT) <= 20);
  assert(netDrive(MURPHY_LUT_20_TEXT) == 0 && netDrive(MURPHY_LUT_21_TEXT) == 0);  // VCOM and rest
  assert(netDrive(MURPHY_LUT_23_TEXT) == 0);
  assert(netDrive(MURPHY_LUT_22_TEXT) == netDrive(MURPHY_LUT_22_FAST));  // FAST's drives to white
  assert(netDrive(MURPHY_LUT_24_TEXT) == 15);                            // and to black
}

void testOnePassPicksEachPixelsDrive() {
  const auto cfg = withText();
  const Bits white = blank(0xFF), black = blank(0x00);
  struct Case {
    const Bits* shown;
    bool shownGray;  // the glass holds one-pass gray over `shown`
    const Bits* page;
    const Bits* mask;
    uint8_t dtm1, dtm2;
  } cases[] = {
      {&white, false, &white, &black, 0xFF, 0xFF},  // stays white: rests
      {&black, false, &black, &black, 0xFF, 0xFF},  // stays black: rests
      {&black, false, &white, &black, 0x00, 0xFF},  // to white
      {&white, false, &black, &black, 0x00, 0x00},  // to black
      {&white, false, &black, &white, 0xFF, 0x00},  // gray, from anything
      {&black, false, &black, &white, 0xFF, 0x00},
      {&black, true, &black, &black, 0x00, 0x00},   // gray on the glass is not black: driven
      {&black, true, &white, &black, 0x00, 0xFF},   // and to white
      {&black, true, &black, &white, 0xFF, 0x00},   // gray that stays gray takes the gray row again
  };
  for (const auto& c : cases) {
    Uc8253MurphyDriver driver(cfg);
    EpdBus bus;
    driver.begin(bus);
    assert(driver.supportsOnePassText() && !driver.supportsGrayFrame());
    driver.setOnePassText(true);
    assert(driver.supportsGrayFrame());
    driver.display(bus, c.shown->data(), nullptr, RefreshMode::Half, false);
    if (c.shownGray) {
      driver.copyGrayscaleLsb(bus, white.data());  // the previous page: all gray
      driver.copyGrayscaleMsb(bus, black.data());
      driver.displayGrayFrame(bus, c.shown->data(), RefreshMode::Fast, false);
    }
    driver.copyGrayscaleLsb(bus, c.mask->data());
    driver.copyGrayscaleMsb(bus, black.data());
    bus.clear();
    driver.displayGrayFrame(bus, c.page->data(), RefreshMode::Fast, false);
    assert(count(bus, 0x12) == 1);  // one refresh, no nudge after it
    assert(uniformPlane(bus, 0x10, c.dtm1) && uniformPlane(bus, 0x13, c.dtm2));
    assert(last(bus, 0x23)->bytes == std::vector<uint8_t>(MURPHY_LUT_23_TEXT, MURPHY_LUT_23_TEXT + MURPHY_LUT_LEN_WB));
  }
}

void testOnePassKeepsTwoPassesWhereItMust() {
  const auto cfg = withText();
  const Bits white = blank(0xFF), black = blank(0x00);
  Uc8253MurphyDriver driver(cfg);
  EpdBus bus;
  driver.begin(bus);
  driver.setOnePassText(true);
  // Straight after begin() nothing of ours is on the glass: a HALF and the nudge, never a blind FAST.
  driver.copyGrayscaleLsb(bus, white.data());
  driver.copyGrayscaleMsb(bus, black.data());
  bus.clear();
  driver.displayGrayFrame(bus, black.data(), RefreshMode::Fast, false);
  assert(count(bus, 0x12) == 2);
  assert(last(bus, 0x21)->bytes == std::vector<uint8_t>(MURPHY_LUT_21_GRAY, MURPHY_LUT_21_GRAY + MURPHY_LUT_LEN_WW));
  // A clean refresh keeps its two passes too.
  driver.display(bus, white.data(), nullptr, RefreshMode::Half, false);
  driver.copyGrayscaleLsb(bus, white.data());
  driver.copyGrayscaleMsb(bus, black.data());
  bus.clear();
  driver.displayGrayFrame(bus, black.data(), RefreshMode::Half, false);
  assert(count(bus, 0x12) == 2);
  // Nothing staged: the page alone.
  bus.clear();
  driver.displayGrayFrame(bus, white.data(), RefreshMode::Fast, false);
  assert(count(bus, 0x12) == 1);
}

void testAFastAfterOnePassDrivesItsGrayAway() {
  const auto cfg = withText();
  const Bits white = blank(0xFF), black = blank(0x00);
  Uc8253MurphyDriver driver(cfg);
  EpdBus bus;
  driver.begin(bus);
  driver.setOnePassText(true);
  driver.display(bus, white.data(), nullptr, RefreshMode::Half, false);
  driver.copyGrayscaleLsb(bus, white.data());
  driver.copyGrayscaleMsb(bus, black.data());
  driver.displayGrayFrame(bus, black.data(), RefreshMode::Fast, false);  // a page of gray
  driver.setOnePassText(false);
  assert(!driver.supportsGrayFrame());
  bus.clear();
  driver.display(bus, black.data(), black.data(), RefreshMode::Fast, false);
  assert(uniformPlane(bus, 0x10, 0xFF));  // gray told it was white, so it takes the drive to black
}

// The stub counts a waitBusy() with the number of writes before it: `fired` is that number right after the refresh
// command, so a wait recorded at `fired` is the waveform's.
bool waitedAt(const EpdBus& bus, size_t writes) {
  for (size_t at : bus.busyWaitWriteCounts) {
    if (at == writes) return true;
  }
  return false;
}

void testADeferredPageReturnsOnceFiredAndItsFinishWaitsAndPowersOff() {
  EpdBus bus;
  Uc8253MurphyDriver driver;
  driver.begin(bus);
  assert(driver.supportsAsyncDisplay());
  Bits page = blank(0xFF), next = blank(0xFF);
  next.set(3, 3, false);
  driver.display(bus, page.data(), nullptr, RefreshMode::Half, false);

  bus.clear();
  assert(driver.displayStart(bus, next.data(), page.data(), RefreshMode::Fast, true));
  assert(count(bus, 0x12) == 1 && bus.writes.back().command == 0x12 && count(bus, 0x02) == 0);
  size_t fired = bus.writes.size();
  size_t waits = bus.busyWaitWriteCounts.size();
  assert(waits == 1 && !waitedAt(bus, fired));  // the power on's wait; the waveform's is still owed
  driver.displayFinish(bus, nullptr);            // and takes no host buffer
  assert(bus.busyWaitWriteCounts.size() == waits + 2 && bus.busyWaitWriteCounts[waits] == fired);
  assert(bus.writes.size() == fired + 1 && bus.writes.back().command == 0x02);  // then the power off, and its wait
  driver.displayFinish(bus, nullptr);  // once
  assert(bus.busyWaitWriteCounts.size() == waits + 2 && bus.writes.size() == fired + 1);

  // Not asked to power off: the finish waits and leaves the controller up.
  bus.clear();
  assert(driver.displayStart(bus, page.data(), next.data(), RefreshMode::Fast, false));
  fired = bus.writes.size();
  driver.displayFinish(bus, nullptr);
  assert(waitedAt(bus, fired) && bus.writes.size() == fired);

  // BUSY seen to drop at the start and back up at the finish: the waveform is over, nothing to wait for.
  bus.clear();
  bus.busy = true;
  assert(driver.displayStart(bus, next.data(), page.data(), RefreshMode::Fast, true));
  bus.busy = false;
  fired = bus.writes.size();
  driver.displayFinish(bus, nullptr);
  assert(!waitedAt(bus, fired) && bus.writes.back().command == 0x02 && waitedAt(bus, fired + 1));
  // Still down: the finish waits.
  bus.clear();
  bus.busy = true;
  assert(driver.displayStart(bus, page.data(), next.data(), RefreshMode::Fast, false));
  fired = bus.writes.size();
  driver.displayFinish(bus, nullptr);
  assert(waitedAt(bus, fired));
  bus.busy = false;
}

void testEveryEntryFinishesARefreshInFlightFirst() {
  const auto cfg = withText();
  EpdBus bus;
  Uc8253MurphyDriver driver(cfg);
  driver.begin(bus);
  const Bits white = blank(0xFF), black = blank(0x00);
  Bits page = blank(0xFF);
  page.set(3, 3, false);
  driver.display(bus, white.data(), nullptr, RefreshMode::Half, false);
  // The page in flight asked for a power off: its wait and that power off come before anything the entry sends.
  const auto finishesFirst = [&](const char* what, auto entry) {
    bus.clear();
    assert(driver.displayStart(bus, page.data(), white.data(), RefreshMode::Fast, true));
    const size_t fired = bus.writes.size();
    entry();
    if (!waitedAt(bus, fired) || bus.writes.size() <= fired || bus.writes[fired].command != 0x02) {
      std::cout << "a refresh in flight was not finished before " << what << "\n";
      assert(false);
    }
    return fired;
  };
  finishesFirst("display", [&] { driver.display(bus, white.data(), page.data(), RefreshMode::Fast, false); });
  finishesFirst("displayStart", [&] {
    assert(driver.displayStart(bus, white.data(), page.data(), RefreshMode::Fast, false));
    driver.displayFinish(bus, nullptr);
  });
  size_t fired = finishesFirst("copyGrayscaleLsb", [&] { driver.copyGrayscaleLsb(bus, black.data()); });
  assert(bus.writes.size() == fired + 1);  // staging itself sends nothing
  fired = finishesFirst("copyGrayscaleMsb", [&] { driver.copyGrayscaleMsb(bus, black.data()); });
  assert(bus.writes.size() == fired + 1);
  finishesFirst("displayGray", [&] { driver.displayGray(bus, nullptr, false, nullptr, false); });
  finishesFirst("displayGrayFrame", [&] { driver.displayGrayFrame(bus, white.data(), RefreshMode::Fast, false); });
  finishesFirst("displayGrayFrameStart", [&] {
    assert(driver.displayGrayFrameStart(bus, white.data(), RefreshMode::Fast, false));
    driver.displayFinish(bus, nullptr);
  });
  finishesFirst("deepSleep", [&] {
    driver.deepSleep(bus);
    assert(bus.writes.back().command == 0x07 && count(bus, 0x02) == 1);  // powered off once, by the finish
  });

  // The single push on its own path (planes staged, a record of the glass) resets the controller for its frame:
  // the page in flight has to be over before that. Staged first, as staging collects a refresh itself.
  driver.begin(bus);
  driver.setOnePassText(true);
  driver.display(bus, white.data(), nullptr, RefreshMode::Half, false);
  driver.copyGrayscaleLsb(bus, white.data());
  driver.copyGrayscaleMsb(bus, black.data());
  bus.clear();
  assert(driver.displayStart(bus, page.data(), white.data(), RefreshMode::Fast, true));
  fired = bus.writes.size();
  assert(driver.displayGrayFrameStart(bus, black.data(), RefreshMode::Fast, false));
  assert(waitedAt(bus, fired) && bus.writes[fired].command == 0x02);
  assert(count(bus, 0x12) == 2);  // one refresh more, no nudge: the one-pass path
  assert(last(bus, 0x23)->bytes == std::vector<uint8_t>(MURPHY_LUT_23_TEXT, MURPHY_LUT_23_TEXT + MURPHY_LUT_LEN_WB));
  driver.displayFinish(bus, nullptr);
}

void testANudgeAfterADeferredPageStillTakesTheWarmPath() {
  const auto cfg = withGray();
  EpdBus bus;
  Uc8253MurphyDriver driver(cfg);
  driver.begin(bus);
  Bits page = blank(0xFF);
  assert(driver.displayStart(bus, page.data(), page.data(), RefreshMode::Fast, false));
  bus.clear();
  Bits lsb = blank(0x00), msb = blank(0x00);
  msb.set(5, 7, true);  // light: MSB only
  nudge(driver, bus, lsb, msb);  // the staging collects the page's refresh
  assert(waitedAt(bus, 0));
  assert(count(bus, 0x01) == 0 && count(bus, 0x04) == 0 && count(bus, 0x12) == 1);
  assert(ramBit(last(bus, 0x10)->bytes, 5, 7) && !ramBit(last(bus, 0x13)->bytes, 5, 7));  // roles traded
}

void testADeferredOnePassPageHasItsRecordFromTheStart() {
  const auto cfg = withText();
  const Bits white = blank(0xFF), black = blank(0x00);
  Uc8253MurphyDriver driver(cfg);
  EpdBus bus;
  driver.begin(bus);
  driver.setOnePassText(true);
  driver.display(bus, white.data(), nullptr, RefreshMode::Half, false);
  driver.copyGrayscaleLsb(bus, white.data());  // a page of gray
  driver.copyGrayscaleMsb(bus, black.data());
  bus.clear();
  assert(driver.displayGrayFrameStart(bus, black.data(), RefreshMode::Fast, true));
  assert(count(bus, 0x12) == 1 && bus.writes.back().command == 0x12 && count(bus, 0x02) == 0);
  assert(last(bus, 0x23)->bytes == std::vector<uint8_t>(MURPHY_LUT_23_TEXT, MURPHY_LUT_23_TEXT + MURPHY_LUT_LEN_WB));
  const size_t fired = bus.writes.size();
  assert(!waitedAt(bus, fired));
  // A B/W page arrives while it paints: the push is finished first, and its gray is on record to be driven away.
  driver.display(bus, black.data(), black.data(), RefreshMode::Fast, false);
  assert(waitedAt(bus, fired) && bus.writes[fired].command == 0x02);
  assert(uniformPlane(bus, 0x10, 0xFF));  // gray told it was white, so it takes the drive to black
}

void testADeferredCleanPushSendsItsOwnNudgeFromTheFinish() {
  const auto cfg = withText();
  const Bits white = blank(0xFF), black = blank(0x00);
  Uc8253MurphyDriver driver(cfg);
  EpdBus bus;
  driver.begin(bus);
  driver.setOnePassText(true);
  driver.display(bus, white.data(), nullptr, RefreshMode::Half, false);
  Bits lsb = blank(0x00), msb = blank(0x00);
  msb.set(5, 7, true);  // one light pixel
  driver.copyGrayscaleLsb(bus, lsb.data());
  driver.copyGrayscaleMsb(bus, msb.data());
  bus.clear();
  assert(driver.displayGrayFrameStart(bus, black.data(), RefreshMode::Half, false));
  assert(count(bus, 0x12) == 1 && bus.writes.back().command == 0x12);  // the page, fired
  assert(!std::memcmp(last(bus, 0x22)->bytes.data(), MURPHY_LUT_22_DEFAULT, sizeof(MURPHY_LUT_22_DEFAULT)));
  const size_t fired = bus.writes.size();
  // The next page's planes arrive while the page paints: the nudge goes out first, with this page's planes.
  driver.copyGrayscaleLsb(bus, white.data());
  assert(waitedAt(bus, fired) && count(bus, 0x12) == 2);
  assert(last(bus, 0x21)->bytes == std::vector<uint8_t>(MURPHY_LUT_21_GRAY, MURPHY_LUT_21_GRAY + MURPHY_LUT_LEN_WW));
  assert(count(bus, 0x01) == 1);  // the page's init alone: the nudge runs warm, planes traded
  assert(ramBit(last(bus, 0x10)->bytes, 5, 7) && !ramBit(last(bus, 0x13)->bytes, 5, 7));
  assert(!ramBit(last(bus, 0x10)->bytes, 20, 7));  // not the next page's all-gray plane
  const size_t done = bus.writes.size();
  driver.displayFinish(bus, nullptr);  // nothing is left of it
  assert(bus.writes.size() == done);

  // The blocking form is the two halves in one call.
  driver.display(bus, white.data(), nullptr, RefreshMode::Half, false);
  driver.copyGrayscaleLsb(bus, lsb.data());
  driver.copyGrayscaleMsb(bus, msb.data());
  bus.clear();
  driver.displayGrayFrame(bus, black.data(), RefreshMode::Half, false);
  assert(count(bus, 0x12) == 2);

  // begin() resets the controller: a push in flight is gone with it, and its nudge never follows.
  driver.display(bus, white.data(), nullptr, RefreshMode::Half, false);
  driver.copyGrayscaleLsb(bus, lsb.data());
  driver.copyGrayscaleMsb(bus, msb.data());
  assert(driver.displayGrayFrameStart(bus, black.data(), RefreshMode::Half, false));
  driver.begin(bus);
  bus.clear();
  driver.display(bus, white.data(), nullptr, RefreshMode::Half, false);
  assert(count(bus, 0x12) == 1 && !waitedAt(bus, 0));
}

// The power off between frames (the host's idle policy) against a deferred refresh.
void testThePowerOffBetweenFramesFinishesARefreshInFlightFirst() {
  EpdBus bus;
  Uc8253MurphyDriver driver;
  driver.begin(bus);
  Bits page = blank(0xFF), next = blank(0xFF);
  next.set(3, 3, false);
  driver.display(bus, page.data(), nullptr, RefreshMode::Half, false);

  // The page is still painting: its waveform is waited out, then the booster goes down, once.
  bus.clear();
  assert(driver.displayStart(bus, next.data(), page.data(), RefreshMode::Fast, false));
  size_t fired = bus.writes.size();
  driver.powerOffBetweenFrames(bus);
  assert(waitedAt(bus, fired) && bus.writes.size() == fired + 1 && bus.writes[fired].command == 0x02);
  assert(waitedAt(bus, fired + 1));  // the power off's own wait
  driver.displayFinish(bus, nullptr);  // the host's finish finds nothing left
  driver.powerOffBetweenFrames(bus);   // and a second power off finds the booster down
  assert(bus.writes.size() == fired + 1);

  // The page asked for its own power off: the finish sends it, the idle one adds none.
  bus.clear();
  assert(driver.displayStart(bus, page.data(), next.data(), RefreshMode::Fast, true));
  fired = bus.writes.size();
  driver.powerOffBetweenFrames(bus);
  assert(waitedAt(bus, fired) && count(bus, 0x02) == 1 && bus.writes.size() == fired + 1);

  // Powered off, the next page starts as any page does: reset, init, power on, one refresh.
  bus.clear();
  assert(driver.displayStart(bus, next.data(), page.data(), RefreshMode::Fast, false));
  assert(count(bus, 0x01) == 1 && count(bus, 0x04) == 1 && count(bus, 0x12) == 1);
  assert(!std::memcmp(last(bus, 0x22)->bytes.data(), MURPHY_LUT_22_FAST, sizeof(MURPHY_LUT_22_FAST)));
  driver.displayFinish(bus, nullptr);
  // Deep sleep after a power off does not power off again.
  driver.powerOffBetweenFrames(bus);
  bus.clear();
  driver.deepSleep(bus);
  assert(count(bus, 0x02) == 0 && bus.writes.back().command == 0x07);
}

void testANudgeAfterAPowerOffBetweenFramesTakesTheColdPath() {
  const auto cfg = withGray();
  EpdBus bus;
  Uc8253MurphyDriver driver(cfg);
  driver.begin(bus);
  Bits page = blank(0xFF);
  Bits lsb = blank(0x00), msb = blank(0x00);
  msb.set(5, 7, true);  // light: MSB only
  // A deferred page, collected, then the idle power off: the nudge that follows resets and takes straight planes.
  assert(driver.displayStart(bus, page.data(), page.data(), RefreshMode::Fast, false));
  driver.displayFinish(bus, nullptr);
  driver.powerOffBetweenFrames(bus);
  bus.clear();
  nudge(driver, bus, lsb, msb);
  assert(count(bus, 0x01) == 1 && count(bus, 0x04) == 1 && count(bus, 0x12) == 1);
  assert(!ramBit(last(bus, 0x10)->bytes, 5, 7) && ramBit(last(bus, 0x13)->bytes, 5, 7));

  // The same with the power off arriving while the page still paints.
  driver.display(bus, page.data(), page.data(), RefreshMode::Half, false);
  assert(driver.displayStart(bus, page.data(), page.data(), RefreshMode::Fast, false));
  driver.powerOffBetweenFrames(bus);
  bus.clear();
  nudge(driver, bus, lsb, msb);
  assert(count(bus, 0x01) == 1 && count(bus, 0x04) == 1 && count(bus, 0x12) == 1);
  assert(!ramBit(last(bus, 0x10)->bytes, 5, 7) && ramBit(last(bus, 0x13)->bytes, 5, 7));

  // Without the power off the nudge stays warm, deferred page or not (the control for the two above).
  driver.display(bus, page.data(), page.data(), RefreshMode::Half, false);
  assert(driver.displayStart(bus, page.data(), page.data(), RefreshMode::Fast, false));
  driver.displayFinish(bus, nullptr);
  bus.clear();
  nudge(driver, bus, lsb, msb);
  assert(count(bus, 0x01) == 0 && count(bus, 0x04) == 0 && count(bus, 0x12) == 1);
  assert(ramBit(last(bus, 0x10)->bytes, 5, 7) && !ramBit(last(bus, 0x13)->bytes, 5, 7));
}

void testAPowerOffWhileACleanPushPaintsSendsItsNudgeWarmFirst() {
  const auto cfg = withText();
  const Bits white = blank(0xFF), black = blank(0x00);
  Uc8253MurphyDriver driver(cfg);
  EpdBus bus;
  driver.begin(bus);
  driver.setOnePassText(true);
  driver.display(bus, white.data(), nullptr, RefreshMode::Half, false);
  Bits lsb = blank(0x00), msb = blank(0x00);
  msb.set(5, 7, true);
  driver.copyGrayscaleLsb(bus, lsb.data());
  driver.copyGrayscaleMsb(bus, msb.data());
  bus.clear();
  assert(driver.displayGrayFrameStart(bus, black.data(), RefreshMode::Half, false));
  const size_t fired = bus.writes.size();
  driver.powerOffBetweenFrames(bus);
  // The page's wait, the nudge with the controller still up (no second init, planes traded), then the power off.
  assert(waitedAt(bus, fired) && count(bus, 0x12) == 2 && count(bus, 0x01) == 1);
  assert(last(bus, 0x21)->bytes == std::vector<uint8_t>(MURPHY_LUT_21_GRAY, MURPHY_LUT_21_GRAY + MURPHY_LUT_LEN_WW));
  assert(ramBit(last(bus, 0x10)->bytes, 5, 7) && !ramBit(last(bus, 0x13)->bytes, 5, 7));
  assert(bus.writes.back().command == 0x02 && count(bus, 0x02) == 1);
  const size_t done = bus.writes.size();
  driver.displayFinish(bus, nullptr);
  assert(bus.writes.size() == done);
  // The gray it left is on record: the next FAST page drives it away.
  bus.clear();
  driver.display(bus, black.data(), black.data(), RefreshMode::Fast, false);
  assert(ramBit(last(bus, 0x10)->bytes, 5, 7));    // the gray pixel is told it was white
  assert(!ramBit(last(bus, 0x10)->bytes, 20, 7));  // black that stays black rests
}

void testTheRecordOfTheGlassOutlivesAPowerOffBetweenFrames() {
  const auto cfg = withText();
  const Bits white = blank(0xFF), black = blank(0x00);
  Uc8253MurphyDriver driver(cfg);
  EpdBus bus;
  driver.begin(bus);
  driver.setOnePassText(true);
  driver.display(bus, white.data(), nullptr, RefreshMode::Half, false);
  driver.powerOffBetweenFrames(bus);
  // Powered off, a FAST single push still knows what it replaces: one refresh with the text bank, no nudge.
  driver.copyGrayscaleLsb(bus, white.data());  // a page of gray
  driver.copyGrayscaleMsb(bus, black.data());
  bus.clear();
  assert(driver.displayGrayFrameStart(bus, black.data(), RefreshMode::Fast, false));
  assert(count(bus, 0x01) == 1 && count(bus, 0x04) == 1 && count(bus, 0x12) == 1);
  assert(last(bus, 0x23)->bytes == std::vector<uint8_t>(MURPHY_LUT_23_TEXT, MURPHY_LUT_23_TEXT + MURPHY_LUT_LEN_WB));
  assert(uniformPlane(bus, 0x10, 0xFF) && uniformPlane(bus, 0x13, 0x00));  // white to gray, everywhere
  // The power off arrives while it paints, then the same page again: its gray is on record and takes the gray row
  // once more, as a page that stays gray does with the booster left up.
  driver.powerOffBetweenFrames(bus);
  assert(bus.writes.back().command == 0x02);
  driver.copyGrayscaleLsb(bus, white.data());
  driver.copyGrayscaleMsb(bus, black.data());
  bus.clear();
  assert(driver.displayGrayFrameStart(bus, black.data(), RefreshMode::Fast, false));
  assert(count(bus, 0x12) == 1 && uniformPlane(bus, 0x10, 0xFF) && uniformPlane(bus, 0x13, 0x00));
  driver.displayFinish(bus, nullptr);
  // And a B/W FAST after another power off drives that gray away.
  driver.powerOffBetweenFrames(bus);
  bus.clear();
  driver.display(bus, black.data(), black.data(), RefreshMode::Fast, false);
  assert(uniformPlane(bus, 0x10, 0xFF));
}

int main() {
  BoardConfig::ACTIVE.displayWidth = FB_W;
  BoardConfig::ACTIVE.displayHeight = FB_H;
  assert(CTRL_W == FB_H && CTRL_H == FB_W);
  testFastGivesWhiteTheLongerDriveAndVcomCoversIt();
  testTheHostOwnsTheCadence();
  testABoardCanStillAskTheDriverToPromote();
  testWithoutAGrayBankNothingChanges();
  testGrayBankIsBalancedAndSelectsByPlane();
  testFastDrivesGrayAwayAndOnlyGray();
  testFastWithoutAPreviousFrameIsAFullRefresh();
  testGrayPixelsClearWithoutAPreviousFrame();
  testASecondNudgeWouldLiftGrayFurtherSoItIsRefused();
  testANudgeRightAfterAPageSkipsTheResetAndSwapsThePlanes();
  testAPoweredOffControllerTakesTheFullPathWithStraightPlanes();
  testASleepEndsTheWarmPath();
  testAnAbandonedPassShowsNothing();
  testAFullRefreshClearsGrayWithoutMarks();
  testTextBankIsAsLongAsFastAndGrayNetsToZero();
  testOnePassPicksEachPixelsDrive();
  testOnePassKeepsTwoPassesWhereItMust();
  testAFastAfterOnePassDrivesItsGrayAway();
  testADeferredPageReturnsOnceFiredAndItsFinishWaitsAndPowersOff();
  testEveryEntryFinishesARefreshInFlightFirst();
  testANudgeAfterADeferredPageStillTakesTheWarmPath();
  testADeferredOnePassPageHasItsRecordFromTheStart();
  testADeferredCleanPushSendsItsOwnNudgeFromTheFinish();
  testThePowerOffBetweenFramesFinishesARefreshInFlightFirst();
  testANudgeAfterAPowerOffBetweenFramesTakesTheColdPath();
  testAPowerOffWhileACleanPushPaintsSendsItsNudgeWarmFirst();
  testTheRecordOfTheGlassOutlivesAPowerOffBetweenFrames();
  std::cout << "PASS: UC8253 Murphy gray nudge, gray-aware FAST and deferred refresh\n";
}
