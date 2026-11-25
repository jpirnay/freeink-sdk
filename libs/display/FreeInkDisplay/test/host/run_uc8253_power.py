#!/usr/bin/env python3
"""Compile the real UC8253 X3 driver with a recording bus and host Arduino shims."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parents[1] / "src"
with tempfile.TemporaryDirectory(prefix="uc8253_power-test-") as directory:
    root = Path(directory)
    for folder in ("driver", "bus", "lut"):
        (root / folder).mkdir()
    for name in ("Uc8253X3Driver.cpp", "Uc8253X3Driver.h", "PanelDriver.h"):
        shutil.copy2(SOURCE / "driver" / name, root / "driver" / name)
    shutil.copy2(SOURCE / "lut/Uc8253X3Luts.h", root / "lut/Uc8253X3Luts.h")
    shutil.copy2(SOURCE / "lut/UltraChipDirectGrayLuts.h", root / "lut/UltraChipDirectGrayLuts.h")
    shutil.copy2(SOURCE.parent / "include/GrayscaleCapabilities.h", root / "GrayscaleCapabilities.h")
    panel = root / "driver/PanelDriver.h"
    panel.write_text(panel.read_text().replace("../../include/GrayscaleCapabilities.h", "../GrayscaleCapabilities.h"))
    (root / "Arduino.h").write_text('''#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdio>
#define HIGH 1
#define PROGMEM
#define log_e(...) std::fprintf(stderr, __VA_ARGS__)
inline unsigned long millis() { static unsigned long t; return ++t; }
inline void delay(unsigned long) {}
inline int digitalRead(int) { return 0; }
''')
    (root / "BoardConfig.h").write_text('''#pragma once
namespace BoardConfig {
inline struct { unsigned short displayWidth=792, displayHeight=528; unsigned displaySpiHz=0; } ACTIVE;
}
''')
    (root / "bus/EpdBus.h").write_text('''#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
namespace freeink {
enum class BusyPolarity { X3TwoPhase };
class EpdBus {
  uint8_t command=0;
public:
  std::vector<uint8_t> oldPlane, newPlane, lastBank, rawRegisters;
  unsigned powerOns=0, powerOffs=0, refreshes=0, dtm1Uploads=0;
  // Per refresh: bit 0 set when DTM2 (new) was all white, bit 1 when DTM1 (old) was.
  std::vector<uint8_t> planesAtRefresh;
  // The X3's controller leaves the frame it drew as the "old" plane when a refresh ends, which the driver relies
  // on instead of uploading DTM1 again (Uc8253X3Driver::displayFinish). Modelled at the wait that ends a refresh.
  bool refreshRunning=false;
  void refreshEnds() { if(refreshRunning) { oldPlane=newPlane; refreshRunning=false; } }
  static bool white(const std::vector<uint8_t>& p) { for (uint8_t b : p) if (b != 0xFF) return false; return !p.empty(); }
  void cmd(uint8_t c) {
    command=c; if(c == 4) ++powerOns; if(c == 2) ++powerOffs;
    if(c == 0x12) { ++refreshes; refreshRunning=true; planesAtRefresh.push_back(uint8_t((white(newPlane) ? 1 : 0) | (white(oldPlane) ? 2 : 0))); }
  }
  // cmdNoWait(): the command, its wait left to the next write (the production bus's settle()).
  unsigned noWaits=0;
  void cmdNoWait(uint8_t c, const char* = nullptr, uint16_t = 0) { cmd(c); ++noWaits; }
  void settle() {}
  void cmdData2(uint8_t c, uint8_t a, uint8_t b) { cmd(c); data(a); data(b); }
  void data(uint8_t) {}
  void data(const uint8_t* p, size_t n) {
    if(n == 42 || n == 49) {
      if(command == 0x20) rawRegisters.clear();
      rawRegisters.push_back(command);
      if(command == 0x20) lastBank.assign(p,p+n);
    }
  }
  void cmdData(uint8_t c, const uint8_t* p, size_t n) { cmd(c); data(p,n); }
  void sendPlaneFlipped(uint8_t c, const uint8_t* p, uint16_t h, uint16_t wb) {
    if(c == 0x10) ++dtm1Uploads;
    (c == 0x10 ? oldPlane : newPlane).assign(p,p+size_t(h)*wb);
  }
  void fillPlane(uint8_t c, uint8_t v, uint16_t h, uint16_t wb) {
    (c == 0x10 ? oldPlane : newPlane).assign(size_t(h)*wb,v);
  }
  void reset(int) {}
  void waitBusy(const char*) { refreshEnds(); }
  void waitRefreshComplete(const char*) { refreshEnds(); }
  struct Pins { int8_t busy=0; };
  Pins pins() const { return {}; }
  struct Transaction { void writeBytes(const uint8_t*, size_t) {} void end() {} };
  Transaction beginTxn() { return {}; }
};
}
''')
    (root / "test.cpp").write_text("""
#include <cassert>
#include <vector>
#include <iostream>
#include "driver/Uc8253X3Driver.h"
int main() {
 freeink::EpdBus bus; freeink::Uc8253X3Driver d;
 const auto caps = d.grayscaleCapabilities();
 assert(caps.supported() && caps.stripUploads && !caps.asyncBase && !caps.stagingWhileBusy);
 assert(d.grayscaleCapabilities(freeink::GrayscaleMode::Direct).base == freeink::GrayscaleBase::Combined);
 std::vector<uint8_t> fb(792/8*528, 0xAA); d.begin(bus);
 d.display(bus,fb.data(),nullptr,freeink::RefreshMode::Full,false);
 assert(bus.powerOns == 1);
 d.display(bus,fb.data(),nullptr,freeink::RefreshMode::Full,false);
 assert(bus.powerOns == 1);
 d.display(bus,fb.data(),nullptr,freeink::RefreshMode::Half,true);
 d.display(bus,fb.data(),nullptr,freeink::RefreshMode::Fast,false);
 assert(bus.powerOns == 2);
 for (auto fallback : {freeink::RefreshMode::Half, freeink::RefreshMode::Fast}) {
   const auto before = bus.refreshes;
   d.beginGrayscale(bus, fb.data(), freeink::GrayscaleMode::Direct, fallback, false);
   d.copyGrayscaleLsb(bus, fb.data());
   d.copyGrayscaleMsb(bus, fb.data());
   assert(bus.refreshes == before);
   d.displayGray(bus, fb.data(), false, nullptr, true);
   assert(bus.refreshes == before + 1);
   const auto vcom = freeink::uc8253X3DefaultConfig().directGray->vcom;
   assert(bus.lastBank == std::vector<uint8_t>(vcom, vcom + 42));
   d.cleanupGrayscaleBuffers(bus, fb.data());
   d.display(bus,fb.data(),nullptr,freeink::RefreshMode::Full,false);
   assert(bus.newPlane == fb && bus.oldPlane == fb);
 }
 // A grayscale base after an AA page must not run as a plain differential. The reader's AA
 // pass leaves greys on the glass that cleanupGrayscaleBuffers() restores the RAM around but
 // does not remove; the differential base then gives those pixels the gentle WW settle and the
 // absolute gray pass idles the endpoints, so the page's text outline survives onto the next
 // grayscale image (X3, 2026-09-23). Nor may it go straight to the new base: a _half gives
 // the page's black text the same 24 frames of black as the white around it, and the text
 // shows through a cover's light gray (X3, 2026-11-08). The base scrubs the panel to white
 // first, then runs the differential from that white: two refreshes, the first with both
 // planes white, the second with the new frame against a white DTM1.
 const auto absolute = freeink::GrayscaleMode::Absolute;
 const auto aaPage = [&] {
   d.display(bus,fb.data(),nullptr,freeink::RefreshMode::Fast,false);        // B/W base, as the reader pushes it
   d.copyGrayscaleLsb(bus, fb.data());
   d.copyGrayscaleMsb(bus, fb.data());
   d.displayGray(bus, fb.data(), false, nullptr, false);                      // overlay gc nudge: greys now on glass
   d.cleanupGrayscaleBuffers(bus, fb.data());                                 // RAM restored; glass untouched
 };
 aaPage();
 auto before = bus.refreshes;
 d.beginGrayscale(bus, fb.data(), absolute, freeink::RefreshMode::Half, false);
 assert(bus.refreshes == before + 2);
 assert(bus.planesAtRefresh[before] == 3);                                    // the scrub: both planes white
 assert(bus.planesAtRefresh[before + 1] == 2);                                // the base, from a white DTM1
 assert(bus.oldPlane == fb);                                                  // DTM1 mirrors the base again
 d.copyGrayscaleLsb(bus, fb.data());
 d.copyGrayscaleMsb(bus, fb.data());
 d.displayGray(bus, fb.data(), false, nullptr, true);                         // absolute pass: greys on glass again
 d.cleanupGrayscaleBuffers(bus, fb.data());
 // A half drives every pixel to its target, which is what retires the claim: the next
 // grayscale base may diff again.
 d.display(bus,fb.data(),nullptr,freeink::RefreshMode::Half,false);
 before = bus.refreshes;
 d.beginGrayscale(bus, fb.data(), absolute, freeink::RefreshMode::Half, false);
 assert(bus.refreshes == before + 1);                                         // differential base
 assert(bus.planesAtRefresh[before] == 0);                                    // no scrub
 d.copyGrayscaleLsb(bus, fb.data());
 d.copyGrayscaleMsb(bus, fb.data());
 d.displayGray(bus, fb.data(), false, nullptr, true);
 d.cleanupGrayscaleBuffers(bus, fb.data());
 // A caller that asks for Full keeps the full path: its first refresh writes the new frame.
 aaPage();
 before = bus.refreshes;
 d.beginGrayscale(bus, fb.data(), absolute, freeink::RefreshMode::Full, false);
 assert(bus.refreshes > before + 1 && (bus.planesAtRefresh[before] & 1) == 0);
 // One-pass text composed by the host: one refresh with its bank and both planes as given, then the
 // DTM1 sync to the page. A clean refresh keeps the B/W path.
 {
   freeink::Uc8253X3Driver x;
   freeink::EpdBus b;
   x.begin(b);
   std::vector<uint8_t> page(792 / 8 * 528, 0xF0), mask(792 / 8 * 528, 0x3C);
   x.display(b, page.data(), nullptr, freeink::RefreshMode::Full, false);
   assert(x.supportsOnePassText() && x.composesOnePassText());
   const auto start = b.refreshes;
   assert(x.displayOnePassStart(b, mask.data(), page.data(), freeink::RefreshMode::Fast, false));
   assert(b.refreshes == start + 1 && b.oldPlane == mask && b.newPlane == page);
   const std::vector<uint8_t> rest = {0x00, 24, 0, 0, 0, 1};
   assert(std::vector<uint8_t>(b.lastBank.begin(), b.lastBank.begin() + 6) == rest);
   x.displayFinish(b, page.data());
   assert(b.oldPlane == page);
   const auto half = b.refreshes;
   x.displayOnePassStart(b, mask.data(), page.data(), freeink::RefreshMode::Half, false);
   x.displayFinish(b, page.data());
   assert(b.refreshes == half + 1 && b.oldPlane == page && b.newPlane == page);
   // A one-pass page leaves its text on the glass as well: the gray base after it scrubs first.
   x.displayOnePassStart(b, mask.data(), page.data(), freeink::RefreshMode::Fast, false);
   x.displayFinish(b, page.data());
   const auto base = b.refreshes;
   x.beginGrayscale(b, page.data(), freeink::GrayscaleMode::Absolute, freeink::RefreshMode::Half, false);
   assert(b.refreshes == base + 2 && b.planesAtRefresh[base] == 3 && b.planesAtRefresh[base + 1] == 2);
 }
 // Power-off between frames (the host's idle hook): the booster goes down once, and the next refresh powers it
 // up and keeps the FAST it asked for. A refresh that powered off by itself (turnOff) still makes the next one a
 // HALF, as before. lastBank holds the VCOM table of the bank the last refresh loaded.
 {
   const auto& cfg = freeink::uc8253X3DefaultConfig();
   const std::vector<uint8_t> fastBank(cfg.fast.vcom, cfg.fast.vcom + 42), halfBank(cfg.half.vcom, cfg.half.vcom + 42);
   freeink::Uc8253X3Driver x;
   freeink::EpdBus b;
   x.begin(b);
   std::vector<uint8_t> page(792 / 8 * 528, 0xF0), mask(792 / 8 * 528, 0x3C);
   x.display(b, page.data(), nullptr, freeink::RefreshMode::Full, false);
   x.display(b, page.data(), nullptr, freeink::RefreshMode::Fast, false);
   assert(b.lastBank == fastBank);
   const auto ons = b.powerOns, offs = b.powerOffs;
   x.powerOffBetweenFrames(b);
   assert(b.powerOffs == offs + 1);
   x.powerOffBetweenFrames(b);  // already down: nothing sent
   assert(b.powerOffs == offs + 1);
   x.display(b, page.data(), nullptr, freeink::RefreshMode::Fast, false);
   assert(b.powerOns == ons + 1 && b.lastBank == fastBank);
   // One-pass text after an idle power-off is still one refresh with its own bank.
   x.powerOffBetweenFrames(b);
   const std::vector<uint8_t> rest = {0x00, 24, 0, 0, 0, 1};
   assert(x.displayOnePassStart(b, mask.data(), page.data(), freeink::RefreshMode::Fast, false));
   assert(std::vector<uint8_t>(b.lastBank.begin(), b.lastBank.begin() + 6) == rest);
   // A refresh in flight keeps its rails: its DTM1 sync is still to come.
   const auto pendingOffs = b.powerOffs;
   x.powerOffBetweenFrames(b);
   assert(b.powerOffs == pendingOffs);
   x.displayFinish(b, page.data());
   // turnOff powers down at the end of the refresh; the wake promotion applies to the next one.
   x.display(b, page.data(), nullptr, freeink::RefreshMode::Fast, true);
   x.display(b, page.data(), nullptr, freeink::RefreshMode::Fast, false);
   assert(b.lastBank == halfBank);
   // An idle power-off after it does not carry that promotion on.
   x.powerOffBetweenFrames(b);
   x.display(b, page.data(), nullptr, freeink::RefreshMode::Fast, false);
   assert(b.lastBank == fastBank);
 }
 // DTM1 after a refresh: FAST, HALF and one-pass leave it to the controller and send no plane after the
 // waveform; a full sync still uploads it. Either way the next FAST diffs against the frame just drawn.
 {
   freeink::Uc8253X3Driver x;
   freeink::EpdBus b;
   x.begin(b);
   std::vector<uint8_t> one(792 / 8 * 528, 0xF0), two(792 / 8 * 528, 0x0F), mask(792 / 8 * 528, 0x3C);
   auto uploads = b.dtm1Uploads;
   x.display(b, one.data(), nullptr, freeink::RefreshMode::Full, false);
   assert(b.dtm1Uploads > uploads && b.oldPlane == one);
   for (auto mode : {freeink::RefreshMode::Fast, freeink::RefreshMode::Half}) {
     uploads = b.dtm1Uploads;
     x.displayStart(b, two.data(), nullptr, mode, false);
     x.displayFinish(b, two.data());
     assert(b.dtm1Uploads == uploads && b.oldPlane == two);
     std::swap(one, two);
   }
   uploads = b.dtm1Uploads;
   assert(x.displayOnePassStart(b, mask.data(), two.data(), freeink::RefreshMode::Fast, false));
   assert(b.dtm1Uploads == uploads + 1 && b.oldPlane == mask);  // the mask goes up as DTM1 before the waveform
   x.displayFinish(b, two.data());
   assert(b.dtm1Uploads == uploads + 1 && b.oldPlane == two);
 }
 // The page turn's early power-on (beginPowerOn) under the sunlight fading fix, which powers off after every
 // refresh: the refresh after it sends no PON of its own and picks the waveform it would have picked with the rails
 // down. The power-off is left running (cmdNoWait); the bus waits it out at its next write.
 {
   const auto& cfg = freeink::uc8253X3DefaultConfig();
   const std::vector<uint8_t> fastBank(cfg.fast.vcom, cfg.fast.vcom + 42), halfBank(cfg.half.vcom, cfg.half.vcom + 42);
   freeink::Uc8253X3Driver x;
   freeink::EpdBus b;
   x.begin(b);
   std::vector<uint8_t> page(792 / 8 * 528, 0xF0), mask(792 / 8 * 528, 0x3C);
   x.display(b, page.data(), nullptr, freeink::RefreshMode::Full, true);
   auto ons = b.powerOns, offs = b.powerOffs, noWaits = b.noWaits;
   x.display(b, page.data(), nullptr, freeink::RefreshMode::Fast, true);
   assert(b.powerOns == ons + 1 && b.powerOffs == offs + 1 && b.noWaits == noWaits + 1 && b.lastBank == fastBank);
   ons = b.powerOns;
   x.beginPowerOn(b);
   assert(b.powerOns == ons + 1);
   x.beginPowerOn(b);  // already up: nothing
   assert(b.powerOns == ons + 1);
   x.display(b, page.data(), nullptr, freeink::RefreshMode::Fast, true);
   assert(b.powerOns == ons + 1 && b.lastBank == fastBank);
   // One-pass text after an early power-on: still one refresh with its own bank, and no PON of its own.
   x.beginPowerOn(b);
   ons = b.powerOns;
   const std::vector<uint8_t> rest = {0x00, 24, 0, 0, 0, 1};
   assert(x.displayOnePassStart(b, mask.data(), page.data(), freeink::RefreshMode::Fast, true));
   assert(b.powerOns == ons && std::vector<uint8_t>(b.lastBank.begin(), b.lastBank.begin() + 6) == rest);
   offs = b.powerOffs;
   x.displayFinish(b, page.data());
   assert(b.powerOffs == offs + 1);
   // A refresh that keeps its rails after a power-off still gets the wake waveform when the page turn raised them.
   x.beginPowerOn(b);
   x.display(b, page.data(), nullptr, freeink::RefreshMode::Fast, false);
   assert(b.lastBank == halfBank);
   // After the idle power-off it keeps its FAST, early power-on or not.
   x.powerOffBetweenFrames(b);
   ons = b.powerOns;
   x.beginPowerOn(b);
   x.display(b, page.data(), nullptr, freeink::RefreshMode::Fast, false);
   assert(b.powerOns == ons + 1 && b.lastBank == fastBank);
   // Rails up: nothing to start.
   ons = b.powerOns;
   x.beginPowerOn(b);
   assert(b.powerOns == ons);
 }
 std::cout << "PASS: UC8253 cold power-on, warm Full, power-off/wake, idle power-off keeps FAST, gray base scrubs after AA and one-pass, early power-on\\n";
}
""")
    exe = root / "test_uc8253_power"
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra",
                    "-Wno-unused-parameter", "-I"+str(root), str(root / "test.cpp"),
                    str(root / "driver/Uc8253X3Driver.cpp"), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
