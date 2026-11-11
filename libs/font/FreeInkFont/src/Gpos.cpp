// LookupFlag filtering through GDEF ported from Free-Ink/freeink-sdk (PR #197, d2e6f09, by Georg Kaindl). Reworked
// here for the resolved entries, binary-searched GDEF reads, and HarfBuzz's treatment of reserved flag bits, unknown
// glyph classes and an unreadable GDEF header.
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
// covers `left`, whatever the right glyph. Without `matchRight` only that is
// looked at: the lookup skips the right glyph.
int32_t pairAdjustment(View subtable, unsigned left, unsigned right, bool matchRight, bool& found, bool& covered) {
  found = false;
  const unsigned format = subtable.u16(0);
  const unsigned valueFormat1 = subtable.u16(4);
  const unsigned valueFormat2 = subtable.u16(6);
  if (format != 1 && format != 2) return 0;
  if ((valueFormat1 & kXAdvanceBit) == 0) return 0;  // no X-advance data → nothing to kern with
  // Both formats list their first glyphs in the coverage table; format 2
  // resolves every covered left glyph to a (possibly zero) record through its
  // class matrix.
  const int coverage = otl::coverageIndex(subtable.sub(subtable.u16(2)), left);
  if (coverage < 0) return 0;
  covered = true;
  if (!matchRight) return 0;
  const unsigned len1 = valueRecordSize(valueFormat1);
  const unsigned len2 = valueRecordSize(valueFormat2);
  const unsigned advanceAt = xAdvanceOffset(valueFormat1);

  if (format == 1) {
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

// LookupFlag bits that skip glyphs by their GDEF class (OpenType chapter 2).
// RightToLeft (0x0001) only concerns cursive attachment, and the reserved
// bits (0x00E0) are ignored, as HarfBuzz ignores them.
constexpr unsigned kIgnoreBaseGlyphs = 0x0002;
constexpr unsigned kIgnoreLigatures = 0x0004;
constexpr unsigned kIgnoreMarks = 0x0008;
constexpr unsigned kUseMarkFilteringSet = 0x0010;
constexpr unsigned kMarkAttachmentType = 0xFF00;
constexpr unsigned kSkippingFlags =
    kIgnoreBaseGlyphs | kIgnoreLigatures | kIgnoreMarks | kUseMarkFilteringSet | kMarkAttachmentType;

// GDEF's GlyphClassDef values. Any other value counts as unclassified, as in
// HarfBuzz.
enum class GlyphClass : unsigned { Unclassified = 0, Base = 1, Ligature = 2, Mark = 3, Component = 4 };

// The font's GDEF as lookup flags read it. Empty, every glyph unclassified,
// for a font without one or one whose header cannot be read: such a font
// kerns as one without GDEF. Every read is a binary search (OpenType.h), so
// classifying a glyph costs bounded time whatever the counts say.
class Gdef {
 public:
  Gdef(const uint8_t* bytes, size_t size) {
    const View table{bytes, size};
    if (bytes != nullptr && table.has(0, 12) && table.u16(0) == 1) table_ = table;
  }
  bool empty() const { return table_.data == nullptr; }
  GlyphClass glyphClass(unsigned glyph) const {
    return static_cast<GlyphClass>(otl::glyphClass(table_.sub(table_.u16(4)), glyph));
  }
  unsigned markAttachmentClass(unsigned glyph) const { return otl::glyphClass(table_.sub(table_.u16(10)), glyph); }
  // Whether mark glyph set `set` (GDEF 1.2 and later) holds `glyph`. A set the
  // table does not have holds nothing.
  bool inMarkGlyphSet(unsigned set, unsigned glyph) const {
    if (table_.u16(2) < 2 || !table_.has(12, 2)) return false;
    const View sets = table_.sub(table_.u16(12));
    const unsigned count = sets.u16(2);
    if (sets.u16(0) != 1 || set >= count || !sets.has(4, size_t(count) * 4)) return false;
    return otl::coverageIndex(sets.sub(sets.u32(4 + size_t(set) * 4)), glyph) >= 0;
  }

 private:
  View table_;
};

// One glyph of the pair, its GDEF class read at most once a query, and only
// when a lookup has flags to test it against.
struct PairGlyph {
  unsigned glyph;
  bool classRead = false;
  GlyphClass glyphClass = GlyphClass::Unclassified;

  GlyphClass classIn(const Gdef& gdef) {
    if (!classRead) {
      glyphClass = gdef.glyphClass(glyph);
      classRead = true;
    }
    return glyphClass;
  }
};

// Whether a lookup with these flags skips the glyph (HarfBuzz's
// check_glyph_property): a base glyph or ligature by its own flag, a mark by
// IgnoreMarks, else by its mark glyph set or, without one, its mark
// attachment class.
bool skips(const Gdef& gdef, const unsigned flags, const unsigned markSet, PairGlyph& glyph) {
  switch (glyph.classIn(gdef)) {
    case GlyphClass::Base:
      return (flags & kIgnoreBaseGlyphs) != 0;
    case GlyphClass::Ligature:
      return (flags & kIgnoreLigatures) != 0;
    case GlyphClass::Mark:
      break;
    case GlyphClass::Unclassified:
    case GlyphClass::Component:
    default:
      return false;
  }
  if (flags & kIgnoreMarks) return true;
  if (flags & kUseMarkFilteringSet) return !gdef.inMarkGlyphSet(markSet, glyph.glyph);
  if (flags & kMarkAttachmentType) return (gdef.markAttachmentClass(glyph.glyph) & 0xFFu) != flags >> 8;
  return false;
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

// For each kern lookup with a PairPos subtable, in the order they apply:
// `visitLookup(lookup)`, then `visitSubtable(subtable)` for each of those
// subtables.
template <typename VisitLookup, typename VisitSubtable>
void forEachKernLookup(View gpos, const unsigned budget, VisitLookup visitLookup, VisitSubtable visitSubtable) {
  unsigned work = 0;
  uint16_t indices[kMaxKernLookups];
  const unsigned count = kernLookups(gpos, indices, work, budget);
  View lookups = gpos.sub(gpos.u16(8));
  for (unsigned k = 0; k < count; ++k) {
    View lookup = lookups.sub(lookups.u16(2 + size_t(indices[k]) * 2));
    const unsigned type = lookup.u16(0);
    const unsigned subtableCount = lookup.u16(4);
    if ((type != 2 && type != 9) || !lookup.has(6, size_t(subtableCount) * 2)) continue;
    bool visited = false;
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
      if (!visited) visitLookup(lookup);
      visited = true;
      visitSubtable(subtable);
    }
  }
}

// What a shaper does with the kern feature's lookups for one pair: each lookup
// that skips neither glyph adds the adjustment of its first subtable that
// matches the pair.
class Accumulator {
 public:
  Accumulator(unsigned left, unsigned right, const uint8_t* gdefBytes, size_t gdefSize)
      : gdef_(gdefBytes, gdefSize), left_{left}, right_{right} {}

  int32_t total() const { return total_; }
  bool covered() const { return covered_; }

  // A lookup starts: its subtables' matches start afresh, and its flags decide
  // which of them the pair is looked up in.
  void startLookup(View lookup) {
    matched_ = false;
    mode_ = modeFor(lookup);
  }

  void apply(View subtable) {
    if (matched_ || mode_ == Mode::Skip || (mode_ == Mode::CoverOnly && covered_)) return;
    bool found = false;
    const int32_t value = pairAdjustment(subtable, left_.glyph, right_.glyph, mode_ == Mode::Match, found, covered_);
    if (found) {
      total_ += value;
      matched_ = true;
    }
  }

 private:
  // What a lookup does with the pair: kern it, skip it (it skips the left
  // glyph: no right glyph kerns with it), or only find whether it covers the
  // left glyph (it skips just the right one).
  enum class Mode : uint8_t { Match, CoverOnly, Skip };

  Mode modeFor(View lookup) {
    const unsigned flags = lookup.u16(2);
    if ((flags & kSkippingFlags) == 0 || gdef_.empty()) return Mode::Match;
    unsigned markSet = 0;
    if (flags & kUseMarkFilteringSet) {
      // The set's index follows the subtable offsets.
      const size_t at = 6 + size_t(lookup.u16(4)) * 2;
      if (!lookup.has(at, 2)) return Mode::Skip;
      markSet = lookup.u16(at);
    }
    if (skips(gdef_, flags, markSet, left_)) return Mode::Skip;
    return skips(gdef_, flags, markSet, right_) ? Mode::CoverOnly : Mode::Match;
  }

  const Gdef gdef_;
  PairGlyph left_;
  PairGlyph right_;
  int32_t total_ = 0;
  bool covered_ = false;
  bool matched_ = false;  // in the current lookup
  Mode mode_ = Mode::Match;
};

}  // namespace

int32_t PairKernAdjustment(const uint8_t* gposBytes, size_t gposSize, uint32_t leftGlyph, uint32_t rightGlyph,
                           const uint8_t* gdefBytes, size_t gdefSize) {
  if (gposBytes == nullptr || leftGlyph == 0 || rightGlyph == 0) return 0;
  const View gpos{gposBytes, gposSize};
  if (!gpos.has(0, 10)) return 0;
  Accumulator sum(leftGlyph, rightGlyph, gdefBytes, gdefSize);
  forEachKernLookup(
      gpos, kQueryBudget, [&](View lookup) { sum.startLookup(lookup); }, [&](View subtable) { sum.apply(subtable); });
  return sum.total();
}

int32_t PairKernAdjustment(const uint8_t* gposBytes, size_t gposSize, uint32_t leftGlyph, uint32_t rightGlyph) {
  return PairKernAdjustment(gposBytes, gposSize, leftGlyph, rightGlyph, nullptr, 0);
}

unsigned ResolveKernSubtables(const uint8_t* gposBytes, size_t gposSize, uint32_t* subtables, unsigned capacity) {
  if (gposBytes == nullptr) return 0;
  const View gpos{gposBytes, gposSize};
  if (!gpos.has(0, 10)) return 0;
  unsigned count = 0;
  const auto add = [&](View table, const uint32_t mark) {
    if (subtables != nullptr && count < capacity) subtables[count] = uint32_t(table.data - gposBytes) | mark;
    ++count;
  };
  forEachKernLookup(
      gpos, kResolveBudget, [&](View lookup) { add(lookup, kLookupStart); }, [&](View subtable) { add(subtable, 0); });
  return count;
}

int32_t PairKernAdjustment(const uint8_t* gposBytes, size_t gposSize, const uint32_t* subtables, unsigned count,
                           uint32_t leftGlyph, uint32_t rightGlyph, const uint8_t* gdefBytes, size_t gdefSize,
                           bool* covered) {
  Accumulator sum(leftGlyph, rightGlyph, gdefBytes, gdefSize);
  if (gposBytes != nullptr && subtables != nullptr && leftGlyph != 0 && rightGlyph != 0) {
    const View gpos{gposBytes, gposSize};
    for (unsigned i = 0; i < count; ++i) {
      // A lookup whose entry cannot be read still starts afresh, unflagged.
      const View table = gpos.sub(subtables[i] & ~kLookupStart);
      if (subtables[i] & kLookupStart) {
        sum.startLookup(table);
      } else if (table.data != nullptr) {
        sum.apply(table);
      }
    }
  }
  if (covered != nullptr) *covered = sum.covered();
  return sum.total();
}

int32_t PairKernAdjustment(const uint8_t* gposBytes, size_t gposSize, const uint32_t* subtables, unsigned count,
                           uint32_t leftGlyph, uint32_t rightGlyph, bool* covered) {
  return PairKernAdjustment(gposBytes, gposSize, subtables, count, leftGlyph, rightGlyph, nullptr, 0, covered);
}

}  // namespace gpos
}  // namespace font
}  // namespace freeink
