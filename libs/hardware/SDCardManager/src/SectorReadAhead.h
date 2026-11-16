#pragma once

// FreeInk SDK — sectors read ahead of a single-sector read, for SdmmcBlockDevice.
//
// SdFat reads a folder one sector at a time through its one-sector cache, so a path
// lookup is a run of single-sector reads, each its own card command. On an X4 Pro
// (1-bit SDMMC, 40 MHz) a lookup under a large /.crosspoint read 42 sectors that way,
// ~17 ms (2026-11-15). The window reads `kSectors` sectors in one command on a miss and
// serves the reads that follow from them.
//
// The window lives in the device's bounce buffer, which a write or a multi-sector read
// reuses: the device calls invalidate() before either, so no stale sector is served.
// Kept apart from the device so the host tests can run it without ESP-IDF.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace freeink {

class SectorReadAhead {
 public:
  static constexpr size_t kSectors = 8;
  static constexpr size_t kSectorSize = 512;

  // Reads `sector` into `dst` (512 bytes): from the window when it holds the sector, else
  // `readRun(first, buffer, count)` fills `buffer` (kSectors * 512 bytes) with up to
  // kSectors sectors from `sector` on, stopping at `cardSectors`. A run that fails is
  // retried as the single sector, so one unreadable sector ahead fails nothing behind it.
  template <typename ReadRun>
  bool read(const uint32_t sector, uint8_t* dst, const uint32_t cardSectors, uint8_t* buffer, ReadRun&& readRun) {
    if (_count != 0 && sector >= _first && sector - _first < _count) {
      memcpy(dst, buffer + static_cast<size_t>(sector - _first) * kSectorSize, kSectorSize);
      return true;
    }
    _count = 0;
    if (sector >= cardSectors) return false;
    const uint32_t left = cardSectors - sector;
    const uint32_t run = left < kSectors ? left : static_cast<uint32_t>(kSectors);
    if (run > 1 && readRun(sector, buffer, run)) {
      _first = sector;
      _count = run;
      memcpy(dst, buffer, kSectorSize);
      return true;
    }
    if (!readRun(sector, buffer, 1)) return false;
    memcpy(dst, buffer, kSectorSize);
    return true;
  }

  void invalidate() { _count = 0; }

 private:
  uint32_t _first = 0;
  uint32_t _count = 0;  // 0: the window holds nothing
};

}  // namespace freeink
