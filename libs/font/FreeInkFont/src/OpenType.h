#pragma once

// FreeInk SDK — what the OpenType layout readers (Gpos.cpp, Gsub.cpp) share: a bounds-checked view of a table, and
// the coverage and class lookups both table types are built from. Internal to the readers.

#include <stddef.h>
#include <stdint.h>

namespace freeink {
namespace font {
namespace otl {

constexpr uint32_t tag(const char (&t)[5]) {
  return uint32_t(uint8_t(t[0])) << 24 | uint32_t(uint8_t(t[1])) << 16 | uint32_t(uint8_t(t[2])) << 8 | uint8_t(t[3]);
}

// Coverage and class range lists are capped: real fonts compact them into ranges (tens to low hundreds even for
// CJK), and a hostile count must not buy an unbounded search.
constexpr unsigned kMaxRanges = 8192;

struct View {
  const uint8_t* data = nullptr;
  size_t size = 0;

  bool has(size_t offset, size_t count) const { return offset <= size && count <= size - offset; }
  unsigned u16(size_t offset) const { return has(offset, 2) ? unsigned(data[offset]) * 256 + data[offset + 1] : 0; }
  int32_t s16(size_t offset) const { return int32_t(int16_t(u16(offset))); }
  uint32_t u32(size_t offset) const { return has(offset, 4) ? (uint32_t(u16(offset)) << 16) | u16(offset + 2) : 0; }
  // Offset 0 is OpenType's own NULL convention for an absent optional
  // field — treating it as a real offset would alias `this` view's own
  // header as the child table (harmless memory-safety-wise since it's still
  // in bounds, but can manufacture a bogus "match" out of a malformed font).
  View sub(size_t offset) const {
    return (offset != 0 && has(offset, 1)) ? View{data + offset, size - offset} : View{};
  }
};

// The index of `first` among sorted records of `stride` bytes whose range [start, end] is the first two uint16 of each
// record, or -1. OpenType requires range lists sorted by start, so a binary search; an unsorted (malformed) list can
// only miss, never read out of bounds.
inline int findRange(View view, size_t at, unsigned count, size_t stride, unsigned glyph) {
  unsigned low = 0;
  unsigned high = count;
  while (low < high) {
    const unsigned middle = (low + high) / 2;
    const size_t record = at + middle * stride;
    if (glyph < view.u16(record)) {
      high = middle;
    } else if (glyph > view.u16(record + 2)) {
      low = middle + 1;
    } else {
      return int(middle);
    }
  }
  return -1;
}

// CoverageTable lookup (GSUB and GPOS alike): the coverage index of `glyph`, or -1.
inline int coverageIndex(View view, unsigned glyph) {
  const unsigned format = view.u16(0);
  const unsigned rawCount = view.u16(2);
  const unsigned count = rawCount < kMaxRanges ? rawCount : kMaxRanges;
  if (format == 1 && view.has(4, size_t(count) * 2)) {
    unsigned low = 0;
    unsigned high = count;
    while (low < high) {
      const unsigned middle = (low + high) / 2;
      if (view.u16(4 + middle * 2) < glyph)
        low = middle + 1;
      else
        high = middle;
    }
    return low < count && view.u16(4 + low * 2) == glyph ? int(low) : -1;
  }
  if (format == 2 && view.has(4, size_t(count) * 6)) {
    const int range = findRange(view, 4, count, 6, glyph);
    if (range < 0) return -1;
    const size_t record = 4 + size_t(range) * 6;
    return int(view.u16(record + 4) + glyph - view.u16(record));
  }
  return -1;
}

// ClassDefTable lookup: the glyph's class, with 0 (the spec's catch-all
// class) for any glyph the table doesn't list.
inline unsigned glyphClass(View view, unsigned glyph) {
  const unsigned format = view.u16(0);
  if (format == 1) {
    const unsigned start = view.u16(2);
    const unsigned rawCount = view.u16(4);
    const unsigned count = rawCount < kMaxRanges ? rawCount : kMaxRanges;
    if (!view.has(6, size_t(count) * 2)) return 0;
    return (glyph >= start && glyph - start < count) ? view.u16(6 + (glyph - start) * 2) : 0;
  }
  if (format == 2) {
    const unsigned rawCount = view.u16(2);
    const unsigned count = rawCount < kMaxRanges ? rawCount : kMaxRanges;
    if (!view.has(4, size_t(count) * 6)) return 0;
    const int range = findRange(view, 4, count, 6, glyph);
    return range < 0 ? 0 : view.u16(4 + size_t(range) * 6 + 4);
  }
  return 0;
}

}  // namespace otl
}  // namespace font
}  // namespace freeink
