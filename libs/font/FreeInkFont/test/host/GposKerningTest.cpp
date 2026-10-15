// Host test for the GPOS pair-kerning reader (Gpos.h) and FtFont's kerning
// fallback through it. Run via test/host/run_gpos.sh under ASan/UBSan.
//
// Two layers: a synthetic hand-built GPOS blob exercises both PairPos
// formats (1 via an Extension wrapper, 2 via class matrices), activation by
// any script, lookups adding up, the resolved-once API, and malformed-input
// robustness deterministically; the DejaVuSans fixture then proves the FtFont
// plumbing end-to-end, including that stripping the legacy 'kern' table
// leaves kerning working through GPOS alone, and checks the resolved reader
// against the stateless one.
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
// indirection around the real `type`.
std::vector<uint8_t> lookup(uint16_t type, const std::vector<uint8_t>& subtable, bool viaExtension) {
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
  appendU16be(bytes, 0);  // LookupFlag
  appendU16be(bytes, 1);  // SubTableCount
  appendU16be(bytes, 8);  // subtable offset (after this 8-byte header)
  appendBlob(bytes, inner);
  return bytes;
}

// Minimal GPOS: latn/DefaultLangSys activates one 'kern' feature that lists
// both lookups: [0] Extension-wrapped PairPos1 (10,20)->-123, [1] PairPos2
// (30,40)->-45.
std::vector<uint8_t> syntheticGpos() {
  const std::vector<uint8_t> lk0 = lookup(2, pairPos1(10, 20, -123), /*viaExtension=*/true);
  const std::vector<uint8_t> lk1 = lookup(2, pairPos2(30, 40, -45), /*viaExtension=*/false);

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
    uint32_t subtables[8];
    expect(ResolveKernSubtables(gpos.data(), gpos.size(), subtables, 8) == 2, "two subtables resolved");
    expect((subtables[0] & freeink::font::gpos::kLookupStart) && (subtables[1] & freeink::font::gpos::kLookupStart),
           "each starts its own lookup");
    expect(ResolveKernSubtables(gpos.data(), gpos.size(), nullptr, 0) == 2, "counting alone");
    uint32_t one[1] = {0};
    expect(ResolveKernSubtables(gpos.data(), gpos.size(), one, 1) == 2 && one[0] != 0,
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
    expect(freeink::font::gpos::PairKernAdjustment(nullptr, 0, subtables, 2, 10, 20) == 0, "null table, resolved");
    for (size_t len = 0; len <= gpos.size(); ++len) {
      (void)ResolveKernSubtables(gpos.data(), len, subtables, 8);
      (void)freeink::font::gpos::PairKernAdjustment(gpos.data(), len, subtables, 2, 10, 20);
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

      // releaseKerningTable(): queries return 0 afterwards, without a reload.
      ftGposOnly.releaseKerningTable();
      expect(ftGposOnly.kerningGlyphs26_6(ftGposOnly.glyphId('A'), ftGposOnly.glyphId('V'), 32u * 64u) == 0,
             "releaseKerningTable() disables GPOS kerning");
    }

    // Integer Font-API route shares the same fallback.
    expect(ft.kerning('A', 'V', 32, 0) <= 0, "integer kerning() API agrees in sign");

    // Every printable ASCII pair: the resolved reader gives what the stateless
    // one gives.
    FT_Library library = nullptr;
    FT_Face face = nullptr;
    expect(FT_Init_FreeType(&library) == 0 &&
               FT_New_Memory_Face(library, font.data(), FT_Long(font.size()), 0, &face) == 0,
           "FreeType opens the fixture");
    std::vector<uint32_t> subtables(256);
    const unsigned count =
        hasGpos ? ResolveKernSubtables(&font[gposOffset], gposLength, subtables.data(), unsigned(subtables.size())) : 0;
    expect(count <= subtables.size(), "the fixture's subtables fit");
    bool resolvedAgrees = true;
    int kerned = 0;
    for (unsigned l = 0x21; l < 0x7F; ++l) {
      for (unsigned r = 0x21; r < 0x7F; ++r) {
        const unsigned gl = FT_Get_Char_Index(face, l);
        const unsigned gr = FT_Get_Char_Index(face, r);
        if (hasGpos) {
          const int32_t stateless = PairKernAdjustment(&font[gposOffset], gposLength, gl, gr);
          const int32_t resolved = freeink::font::gpos::PairKernAdjustment(&font[gposOffset], gposLength,
                                                                          subtables.data(), count, gl, gr);
          resolvedAgrees = resolvedAgrees && stateless == resolved;
          kerned += stateless != 0;
        }
      }
    }
    expect(resolvedAgrees, "resolved GPOS agrees with the stateless reader on every ASCII pair");
    expect(!hasGpos || kerned > 100, "the fixture's GPOS kerns ASCII pairs (not a vacuous match)");
    printf("info: %u kern subtables resolved, %d ASCII pairs kerned\n", count, kerned);
    FT_Done_Face(face);
    FT_Done_FreeType(library);
  }

  printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
