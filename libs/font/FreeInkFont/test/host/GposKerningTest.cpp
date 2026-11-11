// Host test for the GPOS pair-kerning reader (Gpos.h) and FtFont's kerning
// fallback through it, and for the legacy kern table reader (KernTable.h).
// Run via test/host/run_gpos.sh under ASan/UBSan.
//
// Two layers: synthetic hand-built GPOS, GDEF and kern blobs exercise both
// PairPos formats (1 via an Extension wrapper, 2 via class matrices),
// activation by any script, lookups adding up, the lookup flags GDEF classes
// feed, the resolved-once API, and malformed-input robustness
// deterministically; the DejaVuSans fixture then proves the FtFont plumbing
// end-to-end, including that stripping the legacy 'kern' table leaves kerning
// working through GPOS alone and that GDEF reaches resident and streamed
// faces, and checks the resolved reader against the stateless one and the
// kern table reader against FreeType.
//
// The lookup-flag cases, syntheticGdef(), setPairLookupFlags() and the
// resident/streamed GDEF checks are ported from Free-Ink/freeink-sdk (PR #197,
// d2e6f09, by Georg Kaindl), with this reader's answers where it follows
// HarfBuzz instead (reserved bits, unknown classes, an unreadable GDEF).
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H

#include "FtFont.h"
#include "Gpos.h"
#include "KernTable.h"

using freeink::font::FtFont;

namespace {

std::vector<uint8_t> readFile(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) {
    fprintf(stderr, "cannot open %s\n", path);
    exit(1);
  }
  fseek(f, 0, SEEK_END);
  const long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  std::vector<uint8_t> data(static_cast<size_t>(size));
  if (fread(data.data(), 1, data.size(), f) != data.size()) {
    fprintf(stderr, "short read on %s\n", path);
    exit(1);
  }
  fclose(f);
  return data;
}

int checks = 0;
int failures = 0;
void expect(bool cond, const char* what) {
  ++checks;
  if (!cond) {
    ++failures;
    fprintf(stderr, "FAIL: %s\n", what);
  } else {
    printf("ok: %s\n", what);
  }
}

void appendU16be(std::vector<uint8_t>& bytes, uint16_t value) {
  bytes.push_back(uint8_t(value >> 8));
  bytes.push_back(uint8_t(value));
}
void appendU32be(std::vector<uint8_t>& bytes, uint32_t value) {
  appendU16be(bytes, uint16_t(value >> 16));
  appendU16be(bytes, uint16_t(value));
}
void appendTag(std::vector<uint8_t>& bytes, const char* tag) { bytes.insert(bytes.end(), tag, tag + 4); }
void appendBlob(std::vector<uint8_t>& bytes, const std::vector<uint8_t>& blob) {
  bytes.insert(bytes.end(), blob.begin(), blob.end());
}

// CoverageFormat1 over a sorted glyph list.
std::vector<uint8_t> coverage(std::vector<uint16_t> glyphs) {
  std::vector<uint8_t> bytes;
  appendU16be(bytes, 1);
  appendU16be(bytes, uint16_t(glyphs.size()));
  for (uint16_t g : glyphs) appendU16be(bytes, g);
  return bytes;
}

// ClassDefFormat1 assigning `classValue` to the single glyph `glyph`.
std::vector<uint8_t> classDefSingle(uint16_t glyph, uint16_t classValue) {
  std::vector<uint8_t> bytes;
  appendU16be(bytes, 1);
  appendU16be(bytes, glyph);
  appendU16be(bytes, 1);
  appendU16be(bytes, classValue);
  return bytes;
}

// PairPosFormat1 with one pair (left, right) -> xAdvance, ValueFormat1 =
// X_ADVANCE only.
std::vector<uint8_t> pairPos1(uint16_t left, uint16_t right, int16_t xAdvance) {
  std::vector<uint8_t> bytes;
  const std::vector<uint8_t> cov = coverage({left});
  appendU16be(bytes, 1);                       // PosFormat
  appendU16be(bytes, 12);                      // Coverage offset (after 12-byte header)
  appendU16be(bytes, 0x0004);                  // ValueFormat1: XAdvance
  appendU16be(bytes, 0);                       // ValueFormat2
  appendU16be(bytes, 1);                       // PairSetCount
  appendU16be(bytes, uint16_t(12 + cov.size()));  // PairSet offset
  appendBlob(bytes, cov);
  appendU16be(bytes, 1);  // PairValueCount
  appendU16be(bytes, right);
  appendU16be(bytes, uint16_t(xAdvance));
  return bytes;
}

// PairPosFormat2: 2x2 class matrix, (class1=1, class2=1) -> xAdvance, where
// class 1 holds exactly `left` / `right`; everything else is class 0 with a
// zero record. ValueFormat1 = X_ADVANCE only.
std::vector<uint8_t> pairPos2(uint16_t left, uint16_t right, int16_t xAdvance) {
  std::vector<uint8_t> bytes;
  const std::vector<uint8_t> cov = coverage({left});
  const std::vector<uint8_t> cd1 = classDefSingle(left, 1);
  const std::vector<uint8_t> cd2 = classDefSingle(right, 1);
  const uint16_t records = 16;  // header size; 2x2 records of 2 bytes follow
  appendU16be(bytes, 2);        // PosFormat
  appendU16be(bytes, uint16_t(records + 8));  // Coverage offset
  appendU16be(bytes, 0x0004);   // ValueFormat1: XAdvance
  appendU16be(bytes, 0);        // ValueFormat2
  appendU16be(bytes, uint16_t(records + 8 + cov.size()));               // ClassDef1
  appendU16be(bytes, uint16_t(records + 8 + cov.size() + cd1.size()));  // ClassDef2
  appendU16be(bytes, 2);  // Class1Count
  appendU16be(bytes, 2);  // Class2Count
  appendU16be(bytes, 0);  // [0][0]
  appendU16be(bytes, 0);  // [0][1]
  appendU16be(bytes, 0);  // [1][0]
  appendU16be(bytes, uint16_t(xAdvance));  // [1][1]
  appendBlob(bytes, cov);
  appendBlob(bytes, cd1);
  appendBlob(bytes, cd2);
  return bytes;
}

// Lookup wrapping one subtable, optionally through an Extension (type 9)
// indirection around the real `type`, with its LookupFlag (and the mark
// filtering set's index when the flag asks for one).
std::vector<uint8_t> lookup(uint16_t type, const std::vector<uint8_t>& subtable, bool viaExtension, uint16_t flags = 0,
                            uint16_t markSet = 0) {
  std::vector<uint8_t> inner = subtable;
  uint16_t lookupType = type;
  if (viaExtension) {
    std::vector<uint8_t> ext;
    appendU16be(ext, 1);     // ExtensionPosFormat1
    appendU16be(ext, type);  // wrapped lookup type
    appendU32be(ext, 8);     // 32-bit offset to the wrapped subtable
    appendBlob(ext, subtable);
    inner = ext;
    lookupType = 9;
  }
  std::vector<uint8_t> bytes;
  appendU16be(bytes, lookupType);
  appendU16be(bytes, flags);                    // LookupFlag
  appendU16be(bytes, 1);                        // SubTableCount
  appendU16be(bytes, flags & 0x0010 ? 10 : 8);  // subtable offset (after the header)
  if (flags & 0x0010) appendU16be(bytes, markSet);
  appendBlob(bytes, inner);
  return bytes;
}

// Minimal GPOS: latn/DefaultLangSys activates one 'kern' feature that lists
// both lookups: [0] Extension-wrapped PairPos1 (10,20)->-123, [1] PairPos2
// (30,40)->-45, both with `flags` (and `markSet`).
std::vector<uint8_t> syntheticGpos(uint16_t flags = 0, uint16_t markSet = 0, uint16_t left = 10, uint16_t right = 20) {
  const std::vector<uint8_t> lk0 = lookup(2, pairPos1(left, right, -123), /*viaExtension=*/true, flags, markSet);
  const std::vector<uint8_t> lk1 = lookup(2, pairPos2(30, 40, -45), /*viaExtension=*/false, flags, markSet);

  std::vector<uint8_t> scriptList;
  appendU16be(scriptList, 1);
  appendTag(scriptList, "latn");
  appendU16be(scriptList, 8);  // Script offset
  appendU16be(scriptList, 4);  // DefaultLangSys offset (from Script)
  appendU16be(scriptList, 0);  // LangSysCount
  appendU16be(scriptList, 0);       // LookupOrder
  appendU16be(scriptList, 0xFFFF);  // RequiredFeatureIndex
  appendU16be(scriptList, 1);       // FeatureIndexCount
  appendU16be(scriptList, 0);

  std::vector<uint8_t> featureList;
  appendU16be(featureList, 1);
  appendTag(featureList, "kern");
  appendU16be(featureList, 8);  // Feature offset
  appendU16be(featureList, 0);  // FeatureParams
  appendU16be(featureList, 2);  // LookupIndexCount
  appendU16be(featureList, 0);
  appendU16be(featureList, 1);

  std::vector<uint8_t> lookupList;
  appendU16be(lookupList, 2);
  appendU16be(lookupList, 6);
  appendU16be(lookupList, uint16_t(6 + lk0.size()));
  appendBlob(lookupList, lk0);
  appendBlob(lookupList, lk1);

  std::vector<uint8_t> bytes;
  appendU32be(bytes, 0x00010000);
  appendU16be(bytes, 10);
  appendU16be(bytes, uint16_t(10 + scriptList.size()));
  appendU16be(bytes, uint16_t(10 + scriptList.size() + featureList.size()));
  appendBlob(bytes, scriptList);
  appendBlob(bytes, featureList);
  appendBlob(bytes, lookupList);
  return bytes;
}

// GDEF 1.2 classifying glyphs 10 and 30 as `leftClass` and 20 and 40 as
// `rightClass` (GlyphClassDef in format 1 or 2), glyph 10 in mark attachment
// class 7, and two mark glyph sets: {10, 30} and {20, 40}.
std::vector<uint8_t> syntheticGdef(uint16_t leftClass, uint16_t rightClass, bool format1 = false) {
  std::vector<uint8_t> classes;
  if (format1) {
    appendU16be(classes, 1);
    appendU16be(classes, 10);
    appendU16be(classes, 31);
    for (unsigned glyph = 10; glyph <= 40; ++glyph)
      appendU16be(classes, glyph == 10 || glyph == 30 ? leftClass : glyph == 20 || glyph == 40 ? rightClass : 0);
  } else {
    appendU16be(classes, 2);
    appendU16be(classes, 4);
    for (uint16_t glyph : {10, 20, 30, 40}) {
      appendU16be(classes, glyph);
      appendU16be(classes, glyph);
      appendU16be(classes, glyph == 10 || glyph == 30 ? leftClass : rightClass);
    }
  }
  const auto attachment = classDefSingle(10, 7);
  const auto firstSet = coverage({10, 30});
  const auto secondSet = coverage({20, 40});
  std::vector<uint8_t> bytes;
  appendU32be(bytes, 0x00010002);
  appendU16be(bytes, 14);  // GlyphClassDef
  appendU16be(bytes, 0);   // AttachList
  appendU16be(bytes, 0);   // LigCaretList
  appendU16be(bytes, uint16_t(14 + classes.size()));
  appendU16be(bytes, uint16_t(14 + classes.size() + attachment.size()));
  appendBlob(bytes, classes);
  appendBlob(bytes, attachment);
  appendU16be(bytes, 1);  // MarkGlyphSetsDef format
  appendU16be(bytes, 2);  // set count
  appendU32be(bytes, 12);
  appendU32be(bytes, uint32_t(12 + firstSet.size()));
  appendBlob(bytes, firstSet);
  appendBlob(bytes, secondSet);
  return bytes;
}

// A lookup holding several subtables, in order (no Extension).
std::vector<uint8_t> lookupOf(uint16_t type, const std::vector<std::vector<uint8_t>>& subtables) {
  std::vector<uint8_t> bytes;
  appendU16be(bytes, type);
  appendU16be(bytes, 0);
  appendU16be(bytes, uint16_t(subtables.size()));
  size_t offset = 6 + 2 * subtables.size();
  for (const auto& subtable : subtables) {
    appendU16be(bytes, uint16_t(offset));
    offset += subtable.size();
  }
  for (const auto& subtable : subtables) appendBlob(bytes, subtable);
  return bytes;
}

// A GPOS whose scripts each activate their own 'kern' feature through their
// DefaultLangSys, listing the given lookups.
std::vector<uint8_t> gposWith(const std::vector<std::pair<const char*, std::vector<uint16_t>>>& scripts,
                              const std::vector<std::vector<uint8_t>>& lookups) {
  std::vector<uint8_t> scriptList;
  appendU16be(scriptList, uint16_t(scripts.size()));
  size_t offset = 2 + 6 * scripts.size();
  for (size_t i = 0; i < scripts.size(); ++i) {
    appendTag(scriptList, scripts[i].first);
    appendU16be(scriptList, uint16_t(offset + 12 * i));
  }
  for (size_t i = 0; i < scripts.size(); ++i) {
    appendU16be(scriptList, 4);       // DefaultLangSys offset (from Script)
    appendU16be(scriptList, 0);       // LangSysCount
    appendU16be(scriptList, 0);       // LookupOrder
    appendU16be(scriptList, 0xFFFF);  // RequiredFeatureIndex
    appendU16be(scriptList, 1);       // FeatureIndexCount
    appendU16be(scriptList, uint16_t(i));
  }
  std::vector<uint8_t> featureList;
  appendU16be(featureList, uint16_t(scripts.size()));
  offset = 2 + 6 * scripts.size();
  for (size_t i = 0; i < scripts.size(); ++i) {
    appendTag(featureList, "kern");
    appendU16be(featureList, uint16_t(offset));
    offset += 4 + 2 * scripts[i].second.size();
  }
  for (const auto& script : scripts) {
    appendU16be(featureList, 0);
    appendU16be(featureList, uint16_t(script.second.size()));
    for (const uint16_t index : script.second) appendU16be(featureList, index);
  }
  std::vector<uint8_t> lookupList;
  appendU16be(lookupList, uint16_t(lookups.size()));
  offset = 2 + 2 * lookups.size();
  for (const auto& lookup : lookups) {
    appendU16be(lookupList, uint16_t(offset));
    offset += lookup.size();
  }
  for (const auto& lookup : lookups) appendBlob(lookupList, lookup);

  std::vector<uint8_t> bytes;
  appendU32be(bytes, 0x00010000);
  appendU16be(bytes, 10);
  appendU16be(bytes, uint16_t(10 + scriptList.size()));
  appendU16be(bytes, uint16_t(10 + scriptList.size() + featureList.size()));
  appendBlob(bytes, scriptList);
  appendBlob(bytes, featureList);
  appendBlob(bytes, lookupList);
  return bytes;
}

// The resolved reader's answer for a pair, resolving first.
int32_t resolvedKern(const std::vector<uint8_t>& gpos, uint32_t left, uint32_t right, bool* covered = nullptr) {
  uint32_t subtables[64];
  const unsigned count = freeink::font::gpos::ResolveKernSubtables(gpos.data(), gpos.size(), subtables, 64);
  return freeink::font::gpos::PairKernAdjustment(gpos.data(), gpos.size(), subtables, count < 64 ? count : 64, left,
                                                 right, covered);
}

// A legacy kern table, version 0: one subtable per entry of `subtables`, each (coverage, sorted pairs).
std::vector<uint8_t> kernTableWith(
    const std::vector<std::pair<uint16_t, std::vector<std::pair<uint32_t, int16_t>>>>& subtables) {
  std::vector<uint8_t> bytes;
  appendU16be(bytes, 0);
  appendU16be(bytes, uint16_t(subtables.size()));
  for (const auto& [coverage, pairs] : subtables) {
    appendU16be(bytes, 0);
    appendU16be(bytes, uint16_t(14 + 6 * pairs.size()));
    appendU16be(bytes, coverage);
    appendU16be(bytes, uint16_t(pairs.size()));
    appendU16be(bytes, 0);
    appendU16be(bytes, 0);
    appendU16be(bytes, 0);
    for (const auto& [pair, value] : pairs) {
      appendU32be(bytes, pair);
      appendU16be(bytes, uint16_t(value));
    }
  }
  return bytes;
}

// Both readers' answer for a pair under this GDEF: the per-call walk's, which
// the entries resolved from the same table must give too (when they do not,
// a value no check expects). `covered` is the resolved reader's.
constexpr int32_t kReadersDisagree = INT32_MIN;
int32_t kernWithGdef(const std::vector<uint8_t>& gpos, const uint8_t* gdef, size_t gdefSize, uint32_t left,
                     uint32_t right, bool* covered = nullptr) {
  uint32_t entries[64];
  const unsigned count = freeink::font::gpos::ResolveKernSubtables(gpos.data(), gpos.size(), entries, 64);
  const int32_t resolved = freeink::font::gpos::PairKernAdjustment(
      gpos.data(), gpos.size(), entries, count < 64 ? count : 64, left, right, gdef, gdefSize, covered);
  const int32_t walked = freeink::font::gpos::PairKernAdjustment(gpos.data(), gpos.size(), left, right, gdef, gdefSize);
  if (resolved != walked) {
    fprintf(stderr, "readers disagree on %u/%u: %d resolved, %d per call\n", left, right, int(resolved), int(walked));
    return kReadersDisagree;
  }
  return walked;
}

void testLookupFlags() {
  auto adjustment = [](uint16_t flags, uint16_t leftClass, uint16_t rightClass, uint16_t markSet = 0,
                       bool format1 = false, bool extension = true) {
    const auto gpos = syntheticGpos(flags, markSet);
    const auto gdef = syntheticGdef(leftClass, rightClass, format1);
    return kernWithGdef(gpos, gdef.data(), gdef.size(), extension ? 10 : 30, extension ? 20 : 40);
  };
  for (bool extension : {false, true}) {
    const int expected = extension ? -123 : -45;
    for (bool format1 : {false, true}) {
      expect(adjustment(0, 1, 1, 0, format1, extension) == expected, "an unflagged lookup kerns base glyphs");
      expect(adjustment(1, 1, 1, 0, format1, extension) == expected, "RightToLeft does not change a pair adjustment");
      expect(adjustment(0x000E, 1, 1, 0, format1, extension) == 0, "Caecilia's flags skip base glyph pairs");
      for (uint16_t klass : {1, 2, 3}) {
        const uint16_t flag = uint16_t(1u << klass);
        expect(adjustment(flag, klass, 0, 0, format1, extension) == 0, "the class's flag skips the left glyph");
        expect(adjustment(flag, 0, klass, 0, format1, extension) == 0, "the class's flag skips the right glyph");
      }
      expect(adjustment(0x0008, 1, 1, 0, format1, extension) == expected, "IgnoreMarks keeps base glyph kerning");
      expect(adjustment(0x000E, 4, 4, 0, format1, extension) == expected, "component glyphs are never skipped");
      expect(adjustment(0x000E, 0, 0, 0, format1, extension) == expected, "unclassified glyphs are never skipped");
      expect(adjustment(0x000E, 5, 9, 0, format1, extension) == expected,
             "a class GDEF does not define counts as unclassified, as in HarfBuzz");
    }
  }
  expect(adjustment(0x0700, 3, 1) == -123, "a mark of the lookup's attachment class kerns");
  expect(adjustment(0x0800, 3, 1) == 0, "a mark of another attachment class is skipped");
  expect(adjustment(0x0700, 1, 3) == 0, "the attachment class skips the right glyph too");
  expect(adjustment(0x0010, 3, 1, 0) == -123, "a mark in the lookup's mark glyph set kerns (left)");
  expect(adjustment(0x0010, 3, 1, 1) == 0, "a mark outside the set is skipped (left)");
  expect(adjustment(0x0010, 1, 3, 1) == -123, "a mark in the lookup's mark glyph set kerns (right)");
  expect(adjustment(0x0010, 1, 3, 0) == 0, "a mark outside the set is skipped (right)");
  expect(adjustment(0x0010, 3, 1, 2) == 0, "a set the GDEF does not have holds no mark");
  expect(adjustment(0x0810, 3, 1) == -123, "the mark glyph set wins over the attachment class");
  expect(adjustment(0x0018, 3, 1) == 0, "IgnoreMarks wins over the mark glyph set");
  expect(adjustment(0x0020, 1, 1) == -123, "reserved flag bits change nothing, as in HarfBuzz");
  expect(adjustment(0x0022, 1, 1) == 0, "the flags beside them still apply");

  // Only the lookup whose flags skip a glyph drops out; the others still add up.
  const auto mixed = gposWith(
      {{"latn", {0, 1}}}, {lookup(2, pairPos1(10, 20, -100), false, 0x0008), lookup(2, pairPos1(10, 20, -20), false)});
  const auto rightMark = syntheticGdef(1, 3);
  expect(kernWithGdef(mixed, rightMark.data(), rightMark.size(), 10, 20) == -20,
         "an unflagged lookup kerns beside one that skips the pair");

  // `covered` is about the left glyph alone: a lookup that skips only the right
  // glyph still covers it (another right glyph may kern with it), one that
  // skips the left glyph does not.
  {
    const auto ignoreMarks = syntheticGpos(0x0008);
    bool covered = false;
    expect(kernWithGdef(ignoreMarks, rightMark.data(), rightMark.size(), 10, 20, &covered) == 0 && covered,
           "a skipped right glyph leaves the left one covered");
    const auto leftMark = syntheticGdef(3, 1);
    covered = true;
    expect(kernWithGdef(ignoreMarks, leftMark.data(), leftMark.size(), 10, 20, &covered) == 0 && !covered,
           "a skipped left glyph is covered by no lookup");
  }

  // Without a GDEF, or with one these readers cannot read, every glyph is
  // unclassified: the font kerns as the overloads without GDEF read it.
  const auto flagged = syntheticGpos(0x000E);
  expect(kernWithGdef(flagged, nullptr, 0, 10, 20) == -123, "without GDEF every glyph is unclassified");
  expect(freeink::font::gpos::PairKernAdjustment(flagged.data(), flagged.size(), 10, 20) == -123,
         "the overload without GDEF kerns the same");
  auto gdef = syntheticGdef(1, 1);
  bool shortHeaderKerns = true;
  for (size_t len = 0; len < 12; ++len)
    shortHeaderKerns = shortHeaderKerns && kernWithGdef(flagged, gdef.data(), len, 10, 20) == -123;
  expect(shortHeaderKerns, "a GDEF too short for its header counts as absent");
  auto otherMajor = gdef;
  otherMajor[1] = 2;
  expect(kernWithGdef(flagged, otherMajor.data(), otherMajor.size(), 10, 20) == -123,
         "a GDEF of another major version counts as absent");
  auto version10 = syntheticGdef(3, 1);
  version10[3] = 0;  // GDEF 1.0 has no mark glyph sets
  const auto markSetLookups = syntheticGpos(0x0010, 0);
  expect(kernWithGdef(markSetLookups, version10.data(), version10.size(), 10, 20) == 0,
         "before GDEF 1.2 no mark is in a mark glyph set");

  // Every truncation and single-byte corruption of either table stays in
  // bounds (ASan/UBSan), and the two readers keep agreeing.
  int disagreements = 0;
  for (uint16_t flags : {0x000E, 0x0700, 0x0010, 0x0810}) {
    const auto gpos = syntheticGpos(flags);
    gdef = syntheticGdef(3, 1);
    for (size_t len = 0; len <= gdef.size(); ++len)
      disagreements += kernWithGdef(gpos, gdef.data(), len, 10, 20) == kReadersDisagree;
    for (size_t i = 0; i < gdef.size(); ++i) {
      auto corrupt = gdef;
      corrupt[i] ^= 0xFF;
      disagreements += kernWithGdef(gpos, corrupt.data(), corrupt.size(), 10, 20) == kReadersDisagree;
    }
    for (size_t len = 0; len <= gpos.size(); ++len) {
      const std::vector<uint8_t> cut(gpos.begin(), gpos.begin() + long(len));
      disagreements += kernWithGdef(cut, gdef.data(), gdef.size(), 10, 20) == kReadersDisagree;
    }
    for (size_t i = 0; i < gpos.size(); ++i) {
      auto corrupt = gpos;
      corrupt[i] ^= 0xFF;
      disagreements += kernWithGdef(corrupt, gdef.data(), gdef.size(), 10, 20) == kReadersDisagree;
    }
  }
  expect(disagreements == 0, "flagged GPOS/GDEF truncation and corruption sweeps: in bounds, readers agree");

  // GDEF's class and mark set tables are binary-searched: a glyph in the last
  // of 8192 ranges is found, and a query costs bounded time whatever the counts.
  {
    const unsigned ranges = 8192;
    std::vector<uint8_t> big;
    appendU32be(big, 0x00010002);
    appendU16be(big, 14);                             // GlyphClassDef
    appendU16be(big, 0);                              // AttachList
    appendU16be(big, 0);                              // LigCaretList
    appendU16be(big, 0);                              // MarkAttachClassDef
    appendU16be(big, uint16_t(14 + 4 + ranges * 6));  // MarkGlyphSetsDef
    appendU16be(big, 2);                              // ClassDef format 2: every even glyph a mark
    appendU16be(big, uint16_t(ranges));
    for (unsigned r = 0; r < ranges; ++r) {
      appendU16be(big, uint16_t(r * 2));
      appendU16be(big, uint16_t(r * 2));
      appendU16be(big, 3);
    }
    appendU16be(big, 1);  // MarkGlyphSetsDef format
    appendU16be(big, 1);
    appendU32be(big, 8);  // the set's Coverage, format 2: every even glyph but the last
    appendU16be(big, 2);
    appendU16be(big, uint16_t(ranges - 1));
    for (unsigned r = 0; r < ranges - 1; ++r) {
      appendU16be(big, uint16_t(r * 2));
      appendU16be(big, uint16_t(r * 2));
      appendU16be(big, uint16_t(r));
    }
    const uint16_t last = uint16_t((ranges - 1) * 2);
    const uint16_t inSet = uint16_t(last - 2);
    const auto ignoreMarks = syntheticGpos(0x0008, 0, inSet, last);
    const auto inSetOnly = syntheticGpos(0x0010, 0, inSet, inSet - 2);
    const auto lastNotInSet = syntheticGpos(0x0010, 0, inSet, last);
    const auto start = std::chrono::steady_clock::now();
    bool right = true;
    for (int i = 0; i < 1000; ++i) {
      right = right && kernWithGdef(ignoreMarks, big.data(), big.size(), inSet, last) == 0;
      right = right && kernWithGdef(inSetOnly, big.data(), big.size(), inSet, inSet - 2) == -123;
      right = right && kernWithGdef(lastNotInSet, big.data(), big.size(), inSet, last) == 0;
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    expect(right, "a GDEF of 8192 ranges classifies its last glyphs");
    expect(seconds < 1.0, "a GDEF of 8192 ranges costs bounded time");
  }
}

// KernsByGdefClass(): whether a font's kerning depends on its GDEF, as a cache of pairs kerned without it must know.
void testKernsByGdefClass() {
  using freeink::font::gpos::KernsByGdefClass;
  const auto kerns = [](const std::vector<uint8_t>& gpos, const std::vector<uint8_t>& gdef) {
    return KernsByGdefClass(gpos.data(), gpos.size(), gdef.empty() ? nullptr : gdef.data(), gdef.size());
  };
  const auto bases = syntheticGdef(1, 1);
  expect(!kerns(syntheticGpos(0x000E), {}), "without GDEF nothing depends on it");
  expect(!kerns(syntheticGpos(0), bases), "unflagged lookups do not depend on GDEF");
  expect(!kerns(syntheticGpos(0x0001), bases), "nor do RightToLeft ...");
  expect(!kerns(syntheticGpos(0x0020), bases), "... or the reserved bits");
  expect(kerns(syntheticGpos(0x0002), bases), "a lookup that skips base glyphs depends on GDEF");
  expect(kerns(syntheticGpos(0x0004), bases), "so does one that skips ligatures");
  expect(!kerns(syntheticGpos(0x0008), bases), "IgnoreMarks over pairs that hold no mark changes nothing");
  expect(!kerns(syntheticGpos(0x0700), bases), "nor does a mark attachment class");
  expect(kerns(syntheticGpos(0x0008), syntheticGdef(3, 1)), "IgnoreMarks with a mark in the coverage depends");
  expect(kerns(syntheticGpos(0x0008), syntheticGdef(1, 3)), "IgnoreMarks with a mark as a second glyph depends");
  expect(kerns(syntheticGpos(0x0010, 0), syntheticGdef(1, 3)), "so does a mark filtering set");
  // Format 2 alone: a mark that ClassDef2 classes.
  const auto classPairs = gposWith({{"latn", {0}}}, {lookup(2, pairPos2(30, 40, -45), false, 0x0008)});
  expect(kerns(classPairs, syntheticGdef(1, 3)), "a mark ClassDef2 classes is a second glyph");
  expect(!kerns(classPairs, bases), "format 2 without a mark changes nothing");

  // A coverage of 5 x 65,535 glyphs and a mark past them all: the check stops at its bound and answers true.
  std::vector<uint8_t> huge;
  appendU16be(huge, 1);   // PosFormat 1
  appendU16be(huge, 10);  // Coverage right after this header
  appendU16be(huge, 0x0004);
  appendU16be(huge, 0);
  appendU16be(huge, 0);  // PairSetCount
  appendU16be(huge, 2);  // Coverage format 2
  appendU16be(huge, 5);
  for (int r = 0; r < 5; ++r) {
    appendU16be(huge, 0);
    appendU16be(huge, 0xFFFE);
    appendU16be(huge, 0);
  }
  std::vector<uint8_t> lastIsMark;
  appendU32be(lastIsMark, 0x00010000);
  appendU16be(lastIsMark, 12);
  appendU16be(lastIsMark, 0);
  appendU16be(lastIsMark, 0);
  appendU16be(lastIsMark, 0);
  appendBlob(lastIsMark, classDefSingle(0xFFFF, 3));
  const auto start = std::chrono::steady_clock::now();
  expect(kerns(gposWith({{"latn", {0}}}, {lookup(2, huge, false, 0x0008)}), lastIsMark),
         "past its bound the check answers true");
  expect(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < 1.0,
         "a hostile coverage costs bounded time");
  // Truncated and corrupted tables stay in bounds.
  const auto gpos = syntheticGpos(0x0008);
  const auto gdef = syntheticGdef(1, 3);
  for (size_t len = 0; len <= gdef.size(); ++len) (void)KernsByGdefClass(gpos.data(), gpos.size(), gdef.data(), len);
  for (size_t len = 0; len <= gpos.size(); ++len) (void)KernsByGdefClass(gpos.data(), len, gdef.data(), gdef.size());
  for (size_t i = 0; i < gpos.size(); ++i) {
    auto corrupt = gpos;
    corrupt[i] ^= 0xFF;
    (void)KernsByGdefClass(corrupt.data(), corrupt.size(), gdef.data(), gdef.size());
  }
  expect(true, "KernsByGdefClass truncation and corruption sweeps completed");
}

// sfnt table lookup for plain (non-collection) fonts — enough for fixtures.
bool findTable(const std::vector<uint8_t>& font, const char* tag, size_t& offset, size_t& length) {
  if (font.size() < 12) return false;
  const unsigned numTables = (unsigned(font[4]) << 8) | font[5];
  for (unsigned i = 0; i < numTables; ++i) {
    const size_t record = 12 + size_t(i) * 16;
    if (record + 16 > font.size()) return false;
    if (memcmp(&font[record], tag, 4) != 0) continue;
    offset = (uint32_t(font[record + 8]) << 24) | (uint32_t(font[record + 9]) << 16) |
             (uint32_t(font[record + 10]) << 8) | font[record + 11];
    length = (uint32_t(font[record + 12]) << 24) | (uint32_t(font[record + 13]) << 16) |
             (uint32_t(font[record + 14]) << 8) | font[record + 15];
    return offset <= font.size() && length <= font.size() - offset;
  }
  return false;
}

// Zero the directory length of `tag` so FT_Load_Sfnt_Table reports it absent
// (same technique as the ligature test's stripGsubTable). Returns false when
// the font has no such table.
bool stripTable(std::vector<uint8_t>& font, const char* tag) {
  if (font.size() < 12) return false;
  const unsigned numTables = (unsigned(font[4]) << 8) | font[5];
  for (unsigned i = 0; i < numTables; ++i) {
    const size_t record = 12 + size_t(i) * 16;
    if (record + 16 > font.size()) return false;
    if (memcmp(&font[record], tag, 4) == 0) {
      font[record + 12] = font[record + 13] = font[record + 14] = font[record + 15] = 0;
      return true;
    }
  }
  return false;
}

unsigned long readMemory(void* ctx, unsigned long offset, unsigned char* buffer, unsigned long count) {
  const auto& bytes = *static_cast<const std::vector<uint8_t>*>(ctx);
  if (offset > bytes.size() || count > bytes.size() - offset) return 0;
  if (count) memcpy(buffer, bytes.data() + offset, count);
  return count;
}

// Sets every pair adjustment lookup's LookupFlag, as a font such as Caecilia
// LT ships them (0x000E: skip base glyphs, ligatures and marks).
void setPairLookupFlags(std::vector<uint8_t>& font, uint16_t flags) {
  size_t offset = 0, length = 0;
  if (!findTable(font, "GPOS", offset, length)) return;
  auto u16 = [&](size_t at) { return unsigned(font[at]) * 256 + font[at + 1]; };
  const size_t lookups = offset + u16(offset + 8);
  for (unsigned i = 0; i < u16(lookups); ++i) {
    const size_t lookup = lookups + u16(lookups + 2 + i * 2);
    if (u16(lookup) != 2 && u16(lookup) != 9) continue;
    font[lookup + 2] = uint8_t(flags >> 8);
    font[lookup + 3] = uint8_t(flags);
  }
}

}  // namespace

int main(int argc, char** argv) {
  using freeink::font::gpos::PairKernAdjustment;

  // --- Synthetic blob: deterministic parser checks -------------------------
  const std::vector<uint8_t> gpos = syntheticGpos();
  expect(PairKernAdjustment(gpos.data(), gpos.size(), 10, 20) == -123, "format 1 pair via Extension lookup");
  expect(PairKernAdjustment(gpos.data(), gpos.size(), 30, 40) == -45, "format 2 class pair");
  expect(PairKernAdjustment(gpos.data(), gpos.size(), 10, 10) == 0, "covered left, unmatched right");
  expect(PairKernAdjustment(gpos.data(), gpos.size(), 30, 30) == 0, "format 2 class-0 record is zero");
  expect(PairKernAdjustment(gpos.data(), gpos.size(), 99, 20) == 0, "uncovered left glyph");
  expect(PairKernAdjustment(gpos.data(), gpos.size(), 0, 20) == 0, "glyph 0 never kerns");
  expect(PairKernAdjustment(nullptr, 0, 10, 20) == 0, "null table");

  // Robustness: every truncation and every single-byte corruption must stay
  // in bounds (ASan/UBSan verify) and return without hanging.
  for (size_t len = 0; len <= gpos.size(); ++len) {
    (void)PairKernAdjustment(gpos.data(), len, 10, 20);
  }
  for (size_t i = 0; i < gpos.size(); ++i) {
    std::vector<uint8_t> corrupt = gpos;
    corrupt[i] ^= 0xFF;
    (void)PairKernAdjustment(corrupt.data(), corrupt.size(), 10, 20);
    (void)PairKernAdjustment(corrupt.data(), corrupt.size(), 30, 40);
  }
  expect(true, "truncation/corruption sweep completed");
  testLookupFlags();
  testKernsByGdefClass();

  // --- Lookups and scripts --------------------------------------------------
  using freeink::font::gpos::ResolveKernSubtables;
  {
    // Two lookups kerning the same pair: a shaper applies both.
    const std::vector<uint8_t> summed = gposWith({{"latn", {0, 1}}}, {lookup(2, pairPos1(10, 20, -100), false),
                                                                      lookup(2, pairPos1(10, 20, -20), false)});
    expect(PairKernAdjustment(summed.data(), summed.size(), 10, 20) == -120, "the kern feature's lookups add up");
    expect(resolvedKern(summed, 10, 20) == -120, "resolved: the lookups add up");

    // Within a lookup, the first subtable that has the pair wins.
    const std::vector<uint8_t> firstWins =
        gposWith({{"latn", {0}}}, {lookupOf(2, {pairPos1(10, 20, -100), pairPos1(10, 20, -50)})});
    expect(PairKernAdjustment(firstWins.data(), firstWins.size(), 10, 20) == -100,
           "the first matching subtable of a lookup wins");
    expect(resolvedKern(firstWins, 10, 20) == -100, "resolved: the first matching subtable wins");

    // Any script's DefaultLangSys activates its kern feature: Cyrillic kerned
    // under 'cyrl' only.
    const std::vector<uint8_t> scripts =
        gposWith({{"cyrl", {1}}, {"latn", {0}}},
                 {lookup(2, pairPos1(10, 20, -100), false), lookup(2, pairPos1(30, 40, -25), false)});
    expect(PairKernAdjustment(scripts.data(), scripts.size(), 30, 40) == -25, "a cyrl-only kern feature kerns");
    expect(PairKernAdjustment(scripts.data(), scripts.size(), 10, 20) == -100, "latn still kerns beside it");
    expect(resolvedKern(scripts, 30, 40) == -25, "resolved: a cyrl-only kern feature kerns");

    // A lookup listed by two scripts applies once.
    const std::vector<uint8_t> shared =
        gposWith({{"cyrl", {0}}, {"latn", {0}}}, {lookup(2, pairPos1(10, 20, -100), false)});
    expect(PairKernAdjustment(shared.data(), shared.size(), 10, 20) == -100, "a lookup two scripts list applies once");
  }

  // --- Resolved once ----------------------------------------------------------
  {
    using freeink::font::gpos::kLookupStart;
    uint32_t subtables[8];
    expect(ResolveKernSubtables(gpos.data(), gpos.size(), subtables, 8) == 4, "two lookups of one subtable: 4 entries");
    expect((subtables[0] & kLookupStart) && !(subtables[1] & kLookupStart) && (subtables[2] & kLookupStart) &&
               !(subtables[3] & kLookupStart),
           "each lookup's entry leads its subtables");
    expect(ResolveKernSubtables(gpos.data(), gpos.size(), nullptr, 0) == 4, "counting alone");
    uint32_t one[1] = {0};
    expect(ResolveKernSubtables(gpos.data(), gpos.size(), one, 1) == 4 && (one[0] & kLookupStart),
           "a short buffer gets what fits, and the full count");
    const unsigned glyphs[] = {0, 10, 20, 30, 40, 99};
    bool agree = true;
    for (const unsigned left : glyphs) {
      for (const unsigned right : glyphs) {
        agree = agree && resolvedKern(gpos, left, right) == PairKernAdjustment(gpos.data(), gpos.size(), left, right);
      }
    }
    expect(agree, "resolved and stateless agree on every synthetic pair");
    bool covered = false;
    expect(resolvedKern(gpos, 10, 10, &covered) == 0 && covered,
           "a covered left glyph, kerning nothing with this right");
    expect(resolvedKern(gpos, 99, 20, &covered) == 0 && !covered, "an uncovered left glyph");
    expect(freeink::font::gpos::PairKernAdjustment(nullptr, 0, subtables, 4, 10, 20) == 0, "null table, resolved");
    for (size_t len = 0; len <= gpos.size(); ++len) {
      (void)ResolveKernSubtables(gpos.data(), len, subtables, 8);
      (void)freeink::font::gpos::PairKernAdjustment(gpos.data(), len, subtables, 4, 10, 20);
    }
    for (size_t i = 0; i < gpos.size(); ++i) {
      std::vector<uint8_t> corrupt = gpos;
      corrupt[i] ^= 0xFF;
      const unsigned count = ResolveKernSubtables(corrupt.data(), corrupt.size(), subtables, 8);
      (void)freeink::font::gpos::PairKernAdjustment(corrupt.data(), corrupt.size(), subtables, count < 8 ? count : 8,
                                                    10, 20);
    }
    expect(true, "resolved truncation/corruption sweep completed");
  }

  // --- The legacy kern table -------------------------------------------------
  {
    using freeink::font::kerntable::PairKernAdjustment;
    const std::vector<uint8_t> table = kernTableWith({{0x0001, {{(10u << 16) | 20, -40}, {(10u << 16) | 40, 15}}},
                                                     {0x0001, {{(10u << 16) | 20, -5}}},
                                                     {0x0003, {{(10u << 16) | 20, -999}}},  // minimum values
                                                     {0x0005, {{(30u << 16) | 40, -999}}}});  // cross-stream
    bool covered = false;
    expect(PairKernAdjustment(table.data(), table.size(), 10, 20, &covered) == -45 && covered,
           "horizontal subtables add up; minimum and cross-stream ones do not count");
    expect(PairKernAdjustment(table.data(), table.size(), 10, 30, &covered) == 0 && covered,
           "a covered left glyph, kerning nothing with this right");
    expect(PairKernAdjustment(table.data(), table.size(), 30, 40, &covered) == 0 && !covered,
           "only a cross-stream subtable lists it: not covered");
    const std::vector<uint8_t> override =
        kernTableWith({{0x0001, {{(10u << 16) | 20, -40}}}, {0x0009, {{(10u << 16) | 20, -7}}}});
    expect(PairKernAdjustment(override.data(), override.size(), 10, 20) == -7, "an override subtable replaces");
    expect(PairKernAdjustment(nullptr, 0, 10, 20) == 0, "null kern table");
    for (size_t len = 0; len <= table.size(); ++len) (void)PairKernAdjustment(table.data(), len, 10, 20);
    for (size_t i = 0; i < table.size(); ++i) {
      std::vector<uint8_t> corrupt = table;
      corrupt[i] ^= 0xFF;
      (void)PairKernAdjustment(corrupt.data(), corrupt.size(), 10, 20);
    }
    expect(true, "kern table truncation/corruption sweep completed");
  }

  // A hostile table: thousands of scripts sharing one LangSys of 65,535
  // out-of-range feature indices must stay within the walk budget.
  {
    const unsigned scriptCount = 4096;
    const unsigned featureIndexCount = 65535;
    std::vector<uint8_t> hostile;
    appendU16be(hostile, 1);
    appendU16be(hostile, 0);   // version
    appendU16be(hostile, 14);  // ScriptList
    appendU16be(hostile, 10);  // FeatureList
    appendU16be(hostile, 12);  // LookupList
    appendU16be(hostile, 0);   // FeatureCount
    appendU16be(hostile, 0);   // LookupCount
    appendU16be(hostile, uint16_t(scriptCount));
    for (unsigned s = 0; s < scriptCount; ++s) {
      appendTag(hostile, "latn");
      appendU16be(hostile, uint16_t(2 + scriptCount * 6));  // the one shared Script
    }
    appendU16be(hostile, 4);  // DefaultLangSys offset
    appendU16be(hostile, 0);  // LangSysCount
    appendU16be(hostile, 0);  // LookupOrder
    appendU16be(hostile, 0xFFFF);
    appendU16be(hostile, uint16_t(featureIndexCount));
    for (unsigned i = 0; i < featureIndexCount; ++i) appendU16be(hostile, 0xFFFE);

    const auto start = std::chrono::steady_clock::now();
    int32_t sum = 0;
    for (int i = 0; i < 20; ++i) sum += PairKernAdjustment(hostile.data(), hostile.size(), 10, 20);
    uint32_t subtables[8];
    const unsigned resolved = ResolveKernSubtables(hostile.data(), hostile.size(), subtables, 8);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    expect(sum == 0 && resolved == 0, "a hostile table kerns nothing");
    expect(seconds < 1.0, "a hostile table costs bounded time");
  }

  // --- Real font: FtFont fallback end-to-end -------------------------------
  if (argc > 1) {
    const std::vector<uint8_t> font = readFile(argv[1]);

    FtFont ft;
    expect(ft.init(font.data(), uint32_t(font.size()), 32), "fixture font parses");
    const FtFont::GlyphId gidA = ft.glyphId('A');
    const FtFont::GlyphId gidV = ft.glyphId('V');
    expect(gidA != 0 && gidV != 0, "fixture maps A and V");

    size_t gposOffset = 0;
    size_t gposLength = 0;
    const bool hasGpos = findTable(font, "GPOS", gposOffset, gposLength);
    const int32_t gposUnits = hasGpos ? PairKernAdjustment(&font[gposOffset], gposLength, gidA, gidV) : 0;
    printf("info: fixture GPOS %s, A/V adjustment %d font units\n", hasGpos ? "present" : "absent", int(gposUnits));

    const int32_t kern = ft.kerningGlyphs26_6(gidA, gidV, 32u * 64u);
    expect(kern < 0, "A/V kerns negative at 32px (either table)");

    // Strip the legacy 'kern' table: kerning must now flow through GPOS.
    std::vector<uint8_t> gposOnly = font;
    const bool hadLegacy = stripTable(gposOnly, "kern");
    printf("info: legacy kern table %s\n", hadLegacy ? "present (stripped)" : "absent");
    if (gposUnits != 0) {
      FtFont ftGposOnly;
      expect(ftGposOnly.init(gposOnly.data(), uint32_t(gposOnly.size()), 32), "kern-stripped fixture parses");
      const int32_t viaGpos =
          ftGposOnly.kerningGlyphs26_6(ftGposOnly.glyphId('A'), ftGposOnly.glyphId('V'), 32u * 64u);
      expect(viaGpos < 0, "A/V still kerns with legacy kern stripped (GPOS path)");
      printf("info: A/V 26.6 kerning: combined %d, GPOS-only %d\n", int(kern), int(viaGpos));

      // With Caecilia LT's lookup flags (0x000E), the fixture's GDEF classes
      // A and V as base glyphs, so its kern lookups skip them, as HarfBuzz
      // does: A/V kerns 0, resident or streamed. Without its GDEF the same
      // flags skip nothing.
      size_t gdefOffset = 0, gdefLength = 0;
      expect(findTable(font, "GDEF", gdefOffset, gdefLength), "fixture has GDEF glyph classifications");
      auto filtered = gposOnly;
      setPairLookupFlags(filtered, 0x000E);
      FtFont resident;
      expect(resident.init(filtered.data(), uint32_t(filtered.size()), 32), "filtered resident font parses");
      expect(resident.kerning26_6('A', 'V', 32u * 64u) == 0, "resident GDEF filters base-pair lookup");
      printf("info: A/V at 32 px: %.2f px kerned, 0 with Caecilia's lookup flags\n", viaGpos / 64.0);
      expect(resident.init(gposOnly.data(), uint32_t(gposOnly.size()), 32), "resident face reinitializes");
      expect(resident.kerning26_6('A', 'V', 32u * 64u) == viaGpos, "reinit restores unfiltered kerning");
      auto filteredNoGdef = filtered;
      expect(stripTable(filteredNoGdef, "GDEF"), "fixture's GDEF stripped");
      expect(resident.init(filteredNoGdef.data(), uint32_t(filteredNoGdef.size()), 32), "GDEF-stripped font parses");
      expect(resident.kerning26_6('A', 'V', 32u * 64u) == viaGpos, "without GDEF the flags skip nothing");
      size_t flaggedGpos = 0, flaggedGposLength = 0;
      expect(findTable(filtered, "GPOS", flaggedGpos, flaggedGposLength), "flagged fixture has GPOS");
      expect(freeink::font::gpos::KernsByGdefClass(&filtered[flaggedGpos], flaggedGposLength, &filtered[gdefOffset],
                                                   gdefLength),
             "with Caecilia's flags DejaVu's kerning depends on GDEF");
      expect(!freeink::font::gpos::KernsByGdefClass(&font[gposOffset], gposLength, &font[gdefOffset], gdefLength),
             "with its own (none) it does not");

      FtFont stream;
      expect(stream.initStream(readMemory, &filtered, filtered.size(), 32), "filtered streamed font parses");
      expect(stream.kerning26_6('A', 'V', 32u * 64u) == 0, "streamed GDEF filters base-pair lookup");
      stream.setGposByteBudget(gposLength + gdefLength);
      expect(stream.initStream(readMemory, &gposOnly, gposOnly.size(), 32), "streamed face reinitializes");
      expect(stream.kerning26_6('A', 'V', 32u * 64u) == viaGpos, "combined table budget allows streamed kerning");
      stream.setGposByteBudget(gposLength + gdefLength - 1);
      expect(stream.kerning26_6('A', 'V', 32u * 64u) == 0, "lowered combined budget releases both tables");
      expect(stream.initStream(readMemory, &gposOnly, gposOnly.size(), 32), "budget-limited streamed face parses");
      expect(stream.kerning26_6('A', 'V', 32u * 64u) == 0, "combined allocation cannot exceed table budget");
      stream.setGposByteBudget(0);
      expect(stream.initStream(readMemory, &gposOnly, gposOnly.size(), 32), "zero-budget streamed face parses");
      expect(stream.kerning26_6('A', 'V', 32u * 64u) == 0, "zero budget disables streamed GPOS");

      // releaseKerningTable(): queries return 0 afterwards, without a reload.
      ftGposOnly.releaseKerningTable();
      expect(ftGposOnly.kerningGlyphs26_6(ftGposOnly.glyphId('A'), ftGposOnly.glyphId('V'), 32u * 64u) == 0,
             "releaseKerningTable() disables GPOS kerning");
    }

    // Integer Font-API route shares the same fallback.
    expect(ft.kerning('A', 'V', 32, 0) <= 0, "integer kerning() API agrees in sign");

    // Every printable ASCII pair: the resolved reader gives what the stateless
    // one gives, and the kern table reader what FreeType gives.
    FT_Library library = nullptr;
    FT_Face face = nullptr;
    expect(FT_Init_FreeType(&library) == 0 &&
               FT_New_Memory_Face(library, font.data(), FT_Long(font.size()), 0, &face) == 0,
           "FreeType opens the fixture");
    size_t kernOffset = 0;
    size_t kernLength = 0;
    const bool hasKern = findTable(font, "kern", kernOffset, kernLength);
    size_t gdefOffset = 0;
    size_t gdefLength = 0;
    const uint8_t* gdef = findTable(font, "GDEF", gdefOffset, gdefLength) ? &font[gdefOffset] : nullptr;
    std::vector<uint32_t> subtables(256);
    const unsigned count =
        hasGpos ? ResolveKernSubtables(&font[gposOffset], gposLength, subtables.data(), unsigned(subtables.size())) : 0;
    expect(count <= subtables.size(), "the fixture's subtables fit");
    bool resolvedAgrees = true;
    bool gdefChangesNothing = true;
    bool kernAgrees = true;
    int kerned = 0;
    for (unsigned l = 0x21; l < 0x7F; ++l) {
      for (unsigned r = 0x21; r < 0x7F; ++r) {
        const unsigned gl = FT_Get_Char_Index(face, l);
        const unsigned gr = FT_Get_Char_Index(face, r);
        if (hasGpos) {
          const int32_t stateless = PairKernAdjustment(&font[gposOffset], gposLength, gl, gr);
          const int32_t resolved = freeink::font::gpos::PairKernAdjustment(&font[gposOffset], gposLength,
                                                                          subtables.data(), count, gl, gr);
          const int32_t withGdef = PairKernAdjustment(&font[gposOffset], gposLength, gl, gr, gdef, gdefLength);
          const int32_t resolvedWithGdef = freeink::font::gpos::PairKernAdjustment(
              &font[gposOffset], gposLength, subtables.data(), count, gl, gr, gdef, gdefLength);
          resolvedAgrees = resolvedAgrees && stateless == resolved && withGdef == resolvedWithGdef;
          gdefChangesNothing = gdefChangesNothing && withGdef == stateless;
          kerned += stateless != 0;
        }
        if (hasKern) {
          FT_Vector delta{0, 0};
          FT_Get_Kerning(face, gl, gr, FT_KERNING_UNSCALED, &delta);
          kernAgrees = kernAgrees && freeink::font::kerntable::PairKernAdjustment(&font[kernOffset], kernLength, gl,
                                                                                  gr) == int32_t(delta.x);
        }
      }
    }
    expect(resolvedAgrees, "resolved GPOS agrees with the stateless reader on every ASCII pair, GDEF or not");
    // The fixture's kern lookups set no flags: its GDEF changes no pair.
    expect(gdef != nullptr && gdefChangesNothing, "GDEF changes no ASCII pair of a font whose lookups skip nothing");
    expect(!hasGpos || kerned > 100, "the fixture's GPOS kerns ASCII pairs (not a vacuous match)");
    expect(kernAgrees, "the kern table reader agrees with FT_Get_Kerning on every ASCII pair");
    printf("info: %u kern lookup and subtable entries resolved, %d ASCII pairs kerned; legacy kern table %s\n", count,
           kerned, hasKern ? "checked against FreeType" : "absent");
    FT_Done_Face(face);
    FT_Done_FreeType(library);
  }

  printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
