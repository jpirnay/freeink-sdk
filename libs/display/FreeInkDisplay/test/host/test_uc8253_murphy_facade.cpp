// The display facade over the Murphy's UC8253 driver, both real, on the recording bus: a triggered refresh stays
// pending until it is collected, and the end-of-frame power off (FreeInkDisplay::powerOffBetweenFrames, a host's idle
// policy) collects it itself before the booster goes down.
#include <BoardConfig.h>

#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>

#define private public
#include "FreeInkDisplay.h"
#undef private

using namespace freeink;

namespace {
unsigned count(const EpdBus& bus, uint8_t command) {
  unsigned n = 0;
  for (const auto& write : bus.writes) n += write.command == command;
  return n;
}

// A waitBusy() is recorded with the number of writes before it.
bool waitedAt(const EpdBus& bus, size_t writes) {
  for (size_t at : bus.busyWaitWriteCounts) {
    if (at == writes) return true;
  }
  return false;
}
}  // namespace

int main() {
  BoardConfig::ACTIVE.displayWidth = 416;
  BoardConfig::ACTIVE.displayHeight = 240;
  FreeInkDisplay display(1, 2, 3, 4, 5, 6);
  display.begin();
  assert(!std::strcmp(display.getControllerName(), "UC8253"));
  assert(display.supportsAsyncRefresh());
  EpdBus& bus = display._bus;
  const uint32_t size = display.getBufferSize();
  assert(size == 52u * 240u);
  std::memset(display.getFrameBuffer(), 0xFF, size);
  display.displayBuffer(FreeInkDisplay::HALF_REFRESH);
  assert(!display.isRefreshPending());

  // A triggered page: pending, its waveform not waited for. The idle power off waits it out, then powers off, once.
  std::memset(display.getFrameBuffer(), 0xF0, size);
  bus.clear();
  display.triggerDisplay(FreeInkDisplay::FAST_REFRESH);
  assert(display.isRefreshPending() && bus.writes.back().command == 0x12 && count(bus, 0x02) == 0);
  size_t fired = bus.writes.size();
  assert(!waitedAt(bus, fired));
  display.powerOffBetweenFrames();
  assert(!display.isRefreshPending());
  assert(waitedAt(bus, fired) && bus.writes.size() == fired + 1 && bus.writes[fired].command == 0x02);
  display.completeDisplay();
  display.powerOffBetweenFrames();
  assert(bus.writes.size() == fired + 1);

  // Collected by the host first, the power off is all that is left.
  std::memset(display.getFrameBuffer(), 0x0F, size);
  bus.clear();
  display.triggerDisplay(FreeInkDisplay::FAST_REFRESH);
  fired = bus.writes.size();
  display.completeDisplay();
  assert(!display.isRefreshPending() && waitedAt(bus, fired) && bus.writes.size() == fired);
  display.powerOffBetweenFrames();
  assert(bus.writes.size() == fired + 1 && bus.writes.back().command == 0x02);

  // A single push on a clean refresh keeps two passes: the power off sends the nudge it still owes, then powers off.
  display.setOnePassText(true);
  assert(display.supportsGrayFrame());
  std::vector<uint8_t> lsb(size, 0x00), msb(size, 0x00);
  msb[0] = 0x80;
  display.copyGrayscaleLsbBuffers(lsb.data());
  display.copyGrayscaleMsbBuffers(msb.data());
  std::memset(display.getFrameBuffer(), 0x00, size);
  bus.clear();
  display.triggerGrayscaleFrame(FreeInkDisplay::HALF_REFRESH);
  assert(display.isRefreshPending() && count(bus, 0x12) == 1);
  display.powerOffBetweenFrames();
  assert(!display.isRefreshPending() && count(bus, 0x12) == 2 && count(bus, 0x01) == 1);  // the nudge ran warm
  assert(bus.writes.back().command == 0x02 && count(bus, 0x02) == 1);

  // Powered off, the next single push (FAST, one waveform) starts cold and defers as before.
  display.copyGrayscaleLsbBuffers(lsb.data());
  display.copyGrayscaleMsbBuffers(msb.data());
  bus.clear();
  display.triggerGrayscaleFrame(FreeInkDisplay::FAST_REFRESH);
  assert(display.isRefreshPending() && count(bus, 0x01) == 1 && count(bus, 0x04) == 1 && count(bus, 0x12) == 1);
  fired = bus.writes.size();
  // Deep sleep with it in flight: waited out, powered off, then the sleep command.
  display.deepSleep();
  assert(!display.isRefreshPending() && waitedAt(bus, fired) && bus.writes[fired].command == 0x02);
  assert(bus.writes.back().command == 0x07 && count(bus, 0x02) == 1);
  display.setOnePassText(false);

  // A host asks isRefreshPending() before it powers the panel off or lets the chip sleep: the answer is yes from the
  // trigger until the finish has returned, through every wait in it: the page's waveform, the nudge's, each power off.
  display.begin();
  std::memset(display.getFrameBuffer(), 0xFF, size);
  display.displayBuffer(FreeInkDisplay::HALF_REFRESH);
  display.setOnePassText(true);
  display.copyGrayscaleLsbBuffers(lsb.data());
  display.copyGrayscaleMsbBuffers(msb.data());
  std::memset(display.getFrameBuffer(), 0x00, size);
  unsigned waitsSeen = 0, waitsPending = 0;
  bus.onWait = [&] {
    ++waitsSeen;
    waitsPending += display.isRefreshPending();
  };
  display.triggerGrayscaleFrame(FreeInkDisplay::HALF_REFRESH, /*turnOffScreen=*/true);
  assert(display.isRefreshPending());
  const unsigned inStart = waitsSeen;  // the power on: the flag is set once the start has returned
  waitsSeen = waitsPending = 0;
  display.completeDisplay();
  assert(waitsSeen == 3 && waitsPending == 3);  // the page's waveform, the nudge's, its power off
  assert(inStart == 1 && !display.isRefreshPending());
  std::memset(display.getFrameBuffer(), 0xF0, size);
  display.triggerDisplay(FreeInkDisplay::FAST_REFRESH, /*turnOffScreen=*/true);
  waitsSeen = waitsPending = 0;
  display.powerOffBetweenFrames();  // collected by the power off instead of the host's finish
  assert(waitsSeen == 2 && waitsPending == 2 && !display.isRefreshPending());  // the waveform, the page's own power off
  bus.onWait = nullptr;
  display.setOnePassText(false);

  // Inverted output never defers.
  display.begin();
  std::memset(display.getFrameBuffer(), 0xFF, size);
  display.displayBuffer(FreeInkDisplay::HALF_REFRESH);
  display.setInverted(true);
  assert(!display.supportsAsyncRefresh());
  display.triggerDisplay(FreeInkDisplay::FAST_REFRESH);
  assert(!display.isRefreshPending());
  display.triggerDisplayAsync(FreeInkDisplay::FAST_REFRESH);
  assert(!display.isRefreshPending());
  display.setInverted(false);
  std::cout << "PASS: display facade over the UC8253 Murphy driver, deferred refresh and the power off between frames\n";
}
