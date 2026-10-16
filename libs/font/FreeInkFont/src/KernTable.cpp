#include "KernTable.h"

#include "OpenType.h"

namespace freeink {
namespace font {
namespace kerntable {

namespace {

using otl::View;

// A table lists a handful of subtables; more is malformed or hostile.
constexpr unsigned kMaxSubtables = 16;
constexpr unsigned kHorizontal = 0x0001;
constexpr unsigned kMinimum = 0x0002;
constexpr unsigned kCrossStream = 0x0004;
constexpr unsigned kOverride = 0x0008;

}  // namespace

int32_t PairKernAdjustment(const uint8_t* kernBytes, size_t kernSize, uint32_t leftGlyph, uint32_t rightGlyph,
                           bool* covered) {
  bool anyCovered = false;
  int32_t total = 0;
  const View kern{kernBytes, kernBytes != nullptr ? kernSize : 0};
  // Version 0 only: Apple's version 1 table (a 32-bit 0x00010000) is not read, as FreeType does not either.
  if (kern.has(0, 4) && kern.u16(0) == 0 && leftGlyph <= 0xFFFF && rightGlyph <= 0xFFFF) {
    const unsigned tables = kern.u16(2);
    const uint32_t key = leftGlyph << 16 | rightGlyph;
    size_t at = 4;
    for (unsigned t = 0; t < tables && t < kMaxSubtables && kern.has(at, 6); ++t) {
      const unsigned length = kern.u16(at + 2);
      const unsigned coverage = kern.u16(at + 4);
      const View subtable = kern.sub(at);
      at += length;
      if ((coverage >> 8) != 0 || (coverage & (kHorizontal | kMinimum | kCrossStream)) != kHorizontal) {
        if (length < 6) break;
        continue;
      }
      // Format 0: nPairs, three search hints, then (left, right, value) records sorted by left << 16 | right.
      const unsigned pairs = subtable.u16(6);
      if (!subtable.has(14, size_t(pairs) * 6)) break;
      // The first pair at or after (left, 0) says whether the left glyph kerns at all; the second search finds the
      // pair.
      const auto firstAtOrAfter = [&](const uint32_t wanted) {
        unsigned low = 0;
        unsigned high = pairs;
        while (low < high) {
          const unsigned middle = (low + high) / 2;
          if (subtable.u32(14 + size_t(middle) * 6) < wanted)
            low = middle + 1;
          else
            high = middle;
        }
        return low;
      };
      const unsigned first = firstAtOrAfter(leftGlyph << 16);
      if (first < pairs && subtable.u32(14 + size_t(first) * 6) >> 16 == leftGlyph) anyCovered = true;
      const unsigned found = firstAtOrAfter(key);
      if (found < pairs && subtable.u32(14 + size_t(found) * 6) == key) {
        const int32_t value = subtable.s16(14 + size_t(found) * 6 + 4);
        total = (coverage & kOverride) != 0 ? value : total + value;
      }
      if (length < 6) break;
    }
  }
  if (covered != nullptr) *covered = anyCovered;
  return total;
}

}  // namespace kerntable
}  // namespace font
}  // namespace freeink
