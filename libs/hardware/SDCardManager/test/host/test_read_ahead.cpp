// SectorReadAhead over a card model: what single-sector reads return and how many card
// commands they take, with writes and multi-sector reads in between, at the card's end,
// and over a sector that cannot be read.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../../src/SectorReadAhead.h"

namespace {

using freeink::SectorReadAhead;

struct Card {
  explicit Card(const uint32_t sectors) : data(sectors * SectorReadAhead::kSectorSize) {
    for (uint32_t s = 0; s < sectors; ++s) stamp(s, static_cast<uint8_t>(s * 7 + 1));
  }
  std::vector<uint8_t> data;
  unsigned commands = 0;
  uint32_t badSector = UINT32_MAX;

  uint32_t sectors() const { return static_cast<uint32_t>(data.size() / SectorReadAhead::kSectorSize); }
  void stamp(const uint32_t sector, const uint8_t value) {
    memset(&data[sector * SectorReadAhead::kSectorSize], value, SectorReadAhead::kSectorSize);
  }
  bool readRun(const uint32_t first, uint8_t* dst, const uint32_t count) {
    ++commands;
    if (first + count > sectors()) return false;
    if (badSector >= first && badSector < first + count) return false;
    memcpy(dst, &data[first * SectorReadAhead::kSectorSize], count * SectorReadAhead::kSectorSize);
    return true;
  }
};

// The device as it uses the window: a single-sector read goes through it, a write or a
// multi-sector read reuses the buffer and so drops it first.
struct Device {
  explicit Device(Card& card) : card(card) {}
  Card& card;
  SectorReadAhead window;
  uint8_t buffer[SectorReadAhead::kSectors * SectorReadAhead::kSectorSize] = {};

  bool readSector(const uint32_t sector, uint8_t* dst) {
    return window.read(sector, dst, card.sectors(), buffer,
                       [this](uint32_t first, uint8_t* to, uint32_t count) { return card.readRun(first, to, count); });
  }
  bool readSectors(const uint32_t sector, uint8_t* dst, const uint32_t count) {
    window.invalidate();
    memset(buffer, 0xEE, sizeof(buffer));  // the device's multi-sector path runs through the buffer
    return card.readRun(sector, dst, count);
  }
  void writeSector(const uint32_t sector, const uint8_t value) {
    window.invalidate();
    memset(buffer, value, SectorReadAhead::kSectorSize);  // the device copies the sector into the buffer
    card.stamp(sector, value);
    ++card.commands;
  }
};

bool holds(const uint8_t* sector, const uint8_t value) {
  for (size_t i = 0; i < SectorReadAhead::kSectorSize; ++i) {
    if (sector[i] != value) return false;
  }
  return true;
}

uint8_t expected(const uint32_t sector) { return static_cast<uint8_t>(sector * 7 + 1); }

void aFolderScanTakesOneCommandPerEightSectors() {
  Card card(1024);
  Device dev(card);
  uint8_t got[512];
  for (uint32_t s = 100; s < 142; ++s) {
    assert(dev.readSector(s, got));
    assert(holds(got, expected(s)));
  }
  assert(card.commands == 6);  // 42 sectors: 100-107, ..., 140-142
}

void aWriteDropsTheWindow() {
  Card card(1024);
  Device dev(card);
  uint8_t got[512];
  assert(dev.readSector(10, got));
  dev.writeSector(12, 0x5A);
  assert(dev.readSector(12, got));
  assert(holds(got, 0x5A));  // the new bytes, not the window's
  assert(dev.readSector(11, got));
  assert(holds(got, expected(11)));
}

void aMultiSectorReadDropsTheWindow() {
  Card card(1024);
  Device dev(card);
  uint8_t got[512];
  uint8_t run[4 * 512];
  assert(dev.readSector(20, got));
  assert(dev.readSectors(500, run, 4));
  const unsigned before = card.commands;
  assert(dev.readSector(21, got));  // the buffer no longer holds 21
  assert(holds(got, expected(21)));
  assert(card.commands == before + 1);
}

void theWindowStopsAtTheCardsEnd() {
  Card card(1024);
  Device dev(card);
  uint8_t got[512];
  for (uint32_t s = 1020; s < 1024; ++s) {
    assert(dev.readSector(s, got));
    assert(holds(got, expected(s)));
  }
  assert(card.commands == 1);  // 1020-1023 in one run
  assert(!dev.readSector(1024, got));
}

void anUnreadableSectorAheadFailsNothingBehindIt() {
  Card card(1024);
  card.badSector = 205;
  Device dev(card);
  uint8_t got[512];
  assert(dev.readSector(200, got));  // the run fails, the single sector does not
  assert(holds(got, expected(200)));
  assert(!dev.readSector(205, got));
  card.badSector = UINT32_MAX;
  assert(dev.readSector(205, got));
  assert(holds(got, expected(205)));
}

}  // namespace

int main() {
  aFolderScanTakesOneCommandPerEightSectors();
  aWriteDropsTheWindow();
  aMultiSectorReadDropsTheWindow();
  theWindowStopsAtTheCardsEnd();
  anUnreadableSectorAheadFailsNothingBehindIt();
  std::puts("SDMMC read-ahead tests passed");
}
