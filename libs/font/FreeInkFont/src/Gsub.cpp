#include "Gsub.h"

#include <string.h>

#include "OpenType.h"

namespace freeink {
namespace font {
namespace gsub {

namespace {

using otl::coverageIndex;
using otl::View;

constexpr uint32_t kTagLatn = uint32_t('l') << 24 | uint32_t('a') << 16 | uint32_t('t') << 8 | 'n';
constexpr uint32_t kTagDflt = uint32_t('D') << 24 | uint32_t('F') << 16 | uint32_t('L') << 8 | 'T';

}  // namespace

uint32_t LigatureGlyphId(const uint8_t* gsubBytes, size_t gsubSize, const uint32_t* sequence, unsigned length) {
  if (gsubBytes == nullptr || sequence == nullptr) return 0;
  View gsub{gsubBytes, gsubSize};
  if (!gsub.has(0, 10) || length < 2 || length > 3) return 0;
  View scripts = gsub.sub(gsub.u16(4));
  View features = gsub.sub(gsub.u16(6));
  View lookups = gsub.sub(gsub.u16(8));
  if (!scripts.has(0, 2)) return 0;
  const unsigned featureCount = features.u16(0);
  const unsigned lookupCount = lookups.u16(0);
  // Counts are 16-bit fields with no artificial low cap: `.has()` below
  // prevents out-of-bounds reads, while the selected-language walk and the
  // total `work` budget bound the expensive operations. Capping feature
  // indices here would reject ordinary large fonts whose active `liga`
  // feature happens to occur late in the FeatureList.
  if (!features.has(2, size_t(featureCount) * 6) || !lookups.has(2, size_t(lookupCount) * 2)) return 0;

  // A FeatureList is a pool, not an activation list. Only the selected
  // script's DefaultLangSys may activate a feature for this small Latin
  // shaping API. Prefer `latn`; DFLT is the fallback for fonts without a
  // usable Latin DefaultLangSys. Named language systems are intentionally not
  // selected because the API has no language parameter.
  auto usableLangSys = [](View langSys) {
    if (!langSys.has(0, 6)) return false;
    return langSys.has(6, size_t(langSys.u16(4)) * 2);
  };
  const unsigned scriptCount = scripts.u16(0);
  if (!scripts.has(2, size_t(scriptCount) * 6)) return 0;

  // ScriptRecords are required to be sorted by ScriptTag. Use bounded binary
  // searches rather than walking a hostile 65,535-record list on every query;
  // malformed/unsorted input may fail to select a script, but it must still
  // remain bounded and memory-safe.
  unsigned work = 0;
  auto findDefaultLangSys = [&](const uint32_t wanted) -> View {
    unsigned low = 0;
    unsigned high = scriptCount;
    while (low < high) {
      if (++work > 4096) return View{};
      const unsigned middle = low + (high - low) / 2;
      const size_t record = 2 + size_t(middle) * 6;
      const uint32_t tag = scripts.u32(record);
      if (tag < wanted) {
        low = middle + 1;
      } else {
        high = middle;
      }
    }
    if (low >= scriptCount) return View{};
    const size_t record = 2 + size_t(low) * 6;
    if (scripts.u32(record) != wanted) return View{};
    const View script = scripts.sub(scripts.u16(record + 4));
    return script.has(0, 4) ? script.sub(script.u16(0)) : View{};
  };

  const View latinLangSys = findDefaultLangSys(kTagLatn);
  const View dfltLangSys = findDefaultLangSys(kTagDflt);
  const View langSys = usableLangSys(latinLangSys) ? latinLangSys : dfltLangSys;
  if (!usableLangSys(langSys)) return 0;
  const unsigned requiredFeatureIndex = langSys.u16(2);
  const unsigned featureIndexCount = langSys.u16(4);
  if (!langSys.has(6, size_t(featureIndexCount) * 2)) return 0;

  auto processFeature = [&](unsigned featureIndex) -> uint32_t {
    if (featureIndex >= featureCount || ++work > 4096) return 0;
    const size_t record = 2 + featureIndex * 6;
    if (memcmp(features.data + record, "liga", 4) != 0 && memcmp(features.data + record, "rlig", 4) != 0) return 0;
    View feature = features.sub(features.u16(record + 4));
    const unsigned count = feature.u16(2);
    if (!feature.has(4, size_t(count) * 2)) return 0;
    for (unsigned i = 0; i < count; ++i) {
      if (++work > 4096) return 0;
      const unsigned index = feature.u16(4 + i * 2);
      if (index >= lookupCount) continue;
      View lookup = lookups.sub(lookups.u16(2 + index * 2));
      const unsigned type = lookup.u16(0);
      const unsigned subtableCount = lookup.u16(4);
      if ((type != 4 && type != 7) || !lookup.has(6, size_t(subtableCount) * 2)) continue;
      for (unsigned j = 0; j < subtableCount; ++j) {
        if (++work > 4096) return 0;
        View subtable = lookup.sub(lookup.u16(6 + j * 2));
        if (type == 7) {
          // Extension Substitution: format 1 wraps another lookup type at a
          // 32-bit offset (used when the table is too large for 16-bit
          // offsets). Unwrap it if it wraps a ligature-substitution (4).
          if (!subtable.has(0, 8) || subtable.u16(0) != 1 || subtable.u16(2) != 4) continue;
          subtable = subtable.sub(subtable.u32(4));
        }
        if (subtable.u16(0) != 1) continue;  // LigatureSubstFormat1 only
        const int coverage = coverageIndex(subtable.sub(subtable.u16(2)), sequence[0]);
        const unsigned setCount = subtable.u16(4);
        if (coverage < 0 || unsigned(coverage) >= setCount || !subtable.has(6, size_t(setCount) * 2)) continue;
        View set = subtable.sub(subtable.u16(6 + unsigned(coverage) * 2));
        const unsigned ligatureCount = set.u16(0);
        if (!set.has(2, size_t(ligatureCount) * 2)) continue;
        for (unsigned k = 0; k < ligatureCount; ++k) {
          if (++work > 4096) return 0;
          View ligature = set.sub(set.u16(2 + k * 2));
          // LigatureGlyph, LigatureGlyphCount, then (LigatureGlyphCount-1)
          // ComponentGlyphIDs (the coverage glyph itself is implicit).
          if (ligature.u16(2) != length || !ligature.has(4, (length - 1) * 2)) continue;
          bool match = true;
          for (unsigned component = 1; component < length; ++component)
            match = match && ligature.u16(4 + (component - 1) * 2) == sequence[component];
          if (match) return ligature.u16(0);
        }
      }
    }
    return 0;
  };

  if (requiredFeatureIndex != 0xFFFFu) {
    const uint32_t result = processFeature(requiredFeatureIndex);
    if (result != 0) return result;
  }
  for (unsigned i = 0; i < featureIndexCount; ++i) {
    const uint32_t result = processFeature(langSys.u16(6 + i * 2));
    if (result != 0) return result;
  }
  return 0;
}

}  // namespace gsub
}  // namespace font
}  // namespace freeink
