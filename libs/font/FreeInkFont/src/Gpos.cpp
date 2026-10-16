#include "Gpos.h"

#include <string.h>

#include "OpenType.h"

namespace freeink {
namespace font {
namespace gpos {

namespace {

using otl::View;

constexpr uint32_t kTagKern = otl::tag("kern");
// A kern feature lists a handful of lookups; more is malformed or hostile.
constexpr unsigned kMaxKernLookups = 64;
// Reads a walk may make: a hostile table costs bounded time. Resolving is done
// once per font, so it may read more than a single stateless query.
constexpr unsigned kQueryBudget = 4096;
constexpr unsigned kResolveBudget = 65536;

// Every set bit in a ValueFormat contributes one uint16 to its ValueRecord
// (placements, advances, and device-table offsets alike).
unsigned valueRecordSize(unsigned valueFormat) {
  unsigned bits = valueFormat & 0xFFFFu;
  unsigned count = 0;
  while (bits) {
    count += bits & 1u;
    bits >>= 1;
  }
  return count * 2;
}

constexpr unsigned kXAdvanceBit = 0x0004;

// Byte offset of the XAdvance field inside a ValueRecord: one uint16 per set
// bit below it (XPlacement 0x0001, YPlacement 0x0002).
unsigned xAdvanceOffset(unsigned valueFormat) {
  unsigned offset = 0;
  if (valueFormat & 0x0001) offset += 2;
  if (valueFormat & 0x0002) offset += 2;
  return offset;
}

// A PairPos subtable's adjustment for (left, right). `found` distinguishes a
// genuine record (even a zero-valued one — format 2 matrices are dense, and a
// matched pair must stop the lookup's subtables exactly as a shaper's would) from
// "this subtable does not cover the pair". `covered` is set when the subtable
// covers `left`, whatever the right glyph.
int32_t pairAdjustment(View subtable, unsigned left, unsigned right, bool& found, bool& covered) {
  found = false;
  const unsigned format = subtable.u16(0);
  const unsigned valueFormat1 = subtable.u16(4);
  const unsigned valueFormat2 = subtable.u16(6);
  if ((valueFormat1 & kXAdvanceBit) == 0) return 0;  // no X-advance data → nothing to kern with
  const unsigned len1 = valueRecordSize(valueFormat1);
  const unsigned len2 = valueRecordSize(valueFormat2);
  const unsigned advanceAt = xAdvanceOffset(valueFormat1);

  if (format == 1) {
    const int coverage = otl::coverageIndex(subtable.sub(subtable.u16(2)), left);
    if (coverage < 0) return 0;
    covered = true;
    const unsigned setCount = subtable.u16(8);
    if (unsigned(coverage) >= setCount || !subtable.has(10, size_t(setCount) * 2)) return 0;
    View set = subtable.sub(subtable.u16(10 + unsigned(coverage) * 2));
    const unsigned pairCount = set.u16(0);
    const size_t recordSize = 2 + size_t(len1) + len2;
    if (!set.has(2, size_t(pairCount) * recordSize)) return 0;
    // PairValueRecords are sorted by SecondGlyph.
    unsigned low = 0;
    unsigned high = pairCount;
    while (low < high) {
      const unsigned middle = (low + high) / 2;
      if (set.u16(2 + middle * recordSize) < right)
        low = middle + 1;
      else
        high = middle;
    }
    if (low >= pairCount || set.u16(2 + low * recordSize) != right) return 0;
    found = true;
    return set.s16(2 + low * recordSize + 2 + advanceAt);
  }

  if (format == 2) {
    // Coverage lists the first glyphs; a covered left glyph always resolves
    // to a (possibly zero) record through the class matrix.
    if (otl::coverageIndex(subtable.sub(subtable.u16(2)), left) < 0) return 0;
    covered = true;
    const unsigned class1Count = subtable.u16(12);
    const unsigned class2Count = subtable.u16(14);
    const unsigned class1 = otl::glyphClass(subtable.sub(subtable.u16(8)), left);
    const unsigned class2 = otl::glyphClass(subtable.sub(subtable.u16(10)), right);
    if (class1 >= class1Count || class2 >= class2Count) return 0;
    const size_t recordSize = size_t(len1) + len2;
    const size_t record = 16 + (size_t(class1) * class2Count + class2) * recordSize;
    if (!subtable.has(16, size_t(class1Count) * class2Count * recordSize)) return 0;
    found = true;
    return subtable.s16(record + advanceAt);
  }

  return 0;
}

// The lookups the 'kern' feature of any script's DefaultLangSys lists, sorted
// (LookupList order, the order a shaper applies them) and without repeats.
unsigned kernLookups(View gpos, uint16_t (&out)[kMaxKernLookups], unsigned& work, const unsigned budget) {
  View scripts = gpos.sub(gpos.u16(4));
  View features = gpos.sub(gpos.u16(6));
  View lookups = gpos.sub(gpos.u16(8));
  const unsigned scriptCount = scripts.u16(0);
  const unsigned featureCount = features.u16(0);
  const unsigned lookupCount = lookups.u16(0);
  // Counts are 16-bit fields with no artificial low cap: `.has()` prevents
  // out-of-bounds reads, while the `work` budget bounds the walk.
  if (!scripts.has(2, size_t(scriptCount) * 6) || !features.has(2, size_t(featureCount) * 6) ||
      !lookups.has(2, size_t(lookupCount) * 2)) {
    return 0;
  }
  unsigned count = 0;
  const auto addFeature = [&](const unsigned featureIndex) {
    if (++work > budget || featureIndex >= featureCount) return;
    const size_t record = 2 + size_t(featureIndex) * 6;
    if (features.u32(record) != kTagKern) return;
    View feature = features.sub(features.u16(record + 4));
    const unsigned lookupIndexCount = feature.u16(2);
    if (!feature.has(4, size_t(lookupIndexCount) * 2)) return;
    for (unsigned i = 0; i < lookupIndexCount && ++work <= budget; ++i) {
      const unsigned index = feature.u16(4 + i * 2);
      if (index >= lookupCount) continue;
      unsigned at = 0;
      while (at < count && out[at] < index) ++at;
      if ((at < count && out[at] == index) || count == kMaxKernLookups) continue;
      memmove(out + at + 1, out + at, (count - at) * sizeof out[0]);
      out[at] = uint16_t(index);
      ++count;
    }
  };
  for (unsigned s = 0; s < scriptCount && ++work <= budget; ++s) {
    const View script = scripts.sub(scripts.u16(2 + size_t(s) * 6 + 4));
    const View langSys = script.has(0, 4) ? script.sub(script.u16(0)) : View{};
    if (!langSys.has(0, 6)) continue;
    const unsigned requiredFeatureIndex = langSys.u16(2);
    const unsigned featureIndexCount = langSys.u16(4);
    if (!langSys.has(6, size_t(featureIndexCount) * 2)) continue;
    if (requiredFeatureIndex != 0xFFFFu) addFeature(requiredFeatureIndex);
    for (unsigned i = 0; i < featureIndexCount && work <= budget; ++i) addFeature(langSys.u16(6 + i * 2));
  }
  return count;
}

// Calls `visit(subtable, firstOfItsLookup)` for each PairPos subtable of the
// kern lookups, in the order they apply.
template <typename Visit>
void forEachKernSubtable(View gpos, const unsigned budget, Visit visit) {
  unsigned work = 0;
  uint16_t indices[kMaxKernLookups];
  const unsigned count = kernLookups(gpos, indices, work, budget);
  View lookups = gpos.sub(gpos.u16(8));
  for (unsigned k = 0; k < count; ++k) {
    View lookup = lookups.sub(lookups.u16(2 + size_t(indices[k]) * 2));
    const unsigned type = lookup.u16(0);
    const unsigned subtableCount = lookup.u16(4);
    if ((type != 2 && type != 9) || !lookup.has(6, size_t(subtableCount) * 2)) continue;
    bool first = true;
    for (unsigned j = 0; j < subtableCount; ++j) {
      if (++work > budget) return;
      View subtable = lookup.sub(lookup.u16(6 + j * 2));
      if (type == 9) {
        // Extension Positioning: format 1 wraps another lookup type at a
        // 32-bit offset. Unwrap it if it wraps a pair adjustment (2).
        if (!subtable.has(0, 8) || subtable.u16(0) != 1 || subtable.u16(2) != 2) continue;
        subtable = subtable.sub(subtable.u32(4));
      }
      if (subtable.data == nullptr) continue;
      visit(subtable, first);
      first = false;
    }
  }
}

// Adds a subtable's adjustment unless an earlier subtable of the same lookup
// already matched: what a shaper does with the kern feature's lookups.
struct Accumulator {
  int32_t total = 0;
  bool matched = false;  // in the current lookup
  bool covered = false;

  void apply(View subtable, const bool firstOfItsLookup, const unsigned left, const unsigned right) {
    if (firstOfItsLookup) matched = false;
    if (matched) return;
    bool found = false;
    const int32_t value = pairAdjustment(subtable, left, right, found, covered);
    if (found) {
      total += value;
      matched = true;
    }
  }
};

}  // namespace

int32_t PairKernAdjustment(const uint8_t* gposBytes, size_t gposSize, uint32_t leftGlyph, uint32_t rightGlyph) {
  if (gposBytes == nullptr || leftGlyph == 0 || rightGlyph == 0) return 0;
  const View gpos{gposBytes, gposSize};
  if (!gpos.has(0, 10)) return 0;
  Accumulator sum;
  forEachKernSubtable(gpos, kQueryBudget,
                      [&](View subtable, const bool first) { sum.apply(subtable, first, leftGlyph, rightGlyph); });
  return sum.total;
}

unsigned ResolveKernSubtables(const uint8_t* gposBytes, size_t gposSize, uint32_t* subtables, unsigned capacity) {
  if (gposBytes == nullptr) return 0;
  const View gpos{gposBytes, gposSize};
  if (!gpos.has(0, 10)) return 0;
  unsigned count = 0;
  forEachKernSubtable(gpos, kResolveBudget, [&](View subtable, const bool first) {
    if (subtables != nullptr && count < capacity) {
      subtables[count] = uint32_t(subtable.data - gposBytes) | (first ? kLookupStart : 0u);
    }
    ++count;
  });
  return count;
}

int32_t PairKernAdjustment(const uint8_t* gposBytes, size_t gposSize, const uint32_t* subtables, unsigned count,
                           uint32_t leftGlyph, uint32_t rightGlyph, bool* covered) {
  Accumulator sum;
  if (gposBytes != nullptr && subtables != nullptr && leftGlyph != 0 && rightGlyph != 0) {
    const View gpos{gposBytes, gposSize};
    for (unsigned i = 0; i < count; ++i) {
      const bool first = (subtables[i] & kLookupStart) != 0;
      const View subtable = gpos.sub(subtables[i] & ~kLookupStart);
      if (subtable.data != nullptr) {
        sum.apply(subtable, first, leftGlyph, rightGlyph);
      } else if (first) {
        sum.matched = false;  // a lookup whose first subtable cannot be read still starts afresh
      }
    }
  }
  if (covered != nullptr) *covered = sum.covered;
  return sum.total;
}

}  // namespace gpos
}  // namespace font
}  // namespace freeink
