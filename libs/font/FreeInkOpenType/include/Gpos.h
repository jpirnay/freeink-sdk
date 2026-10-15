#pragma once

// FreeInk SDK — minimal OpenType GPOS pair-kerning reader.
//
// FreeType's FT_Get_Kerning() reads only the legacy 'kern' table, but fonts
// built with modern toolchains (fontmake, Glyphs, current Google Fonts
// releases, all CFF/.otf fonts) ship their kerning exclusively in the GPOS
// table's 'kern' feature — interpreting that is normally HarfBuzz's job,
// which is too heavy to vendor here for one lookup type. This reads exactly
// the 'kern' feature's Pair Adjustment lookups (type 2, and 9 wrapping 2) in
// both PairPos formats (1: per-glyph pair sets, 2: class matrices),
// bounds-checked and capped against malformed/hostile font data, in the same
// style as the GSUB ligature reader (see Gsub.h).
//
// The 'kern' feature is the one any script's DefaultLangSys activates (its
// required feature included): Latin, Cyrillic and Greek kern alike, and fonts
// usually point every script at the same lookups. Named language systems are
// not consulted, since this API has no language input. The feature's lookups
// apply as a shaper applies them: in LookupList order, the first subtable
// that matches the pair in each, their adjustments added.
//
// Only the FIRST value record's X-advance adjustment is returned (how much
// the left glyph's advance changes before drawing the right glyph) — that is
// the entirety of horizontal pair kerning. Placement adjustments, second-
// glyph records, and mark/cursive attachment are out of scope.

#include <stddef.h>
#include <stdint.h>

namespace freeink {
namespace font {
namespace gpos {

// Returns the X-advance adjustment for drawing `rightGlyph` after
// `leftGlyph`, in FONT UNITS (FUnits, typically negative), or 0 when no
// active lookup covers the pair (or the table is absent/malformed). Scale by
// the face's x_scale (FT_MulFix) for 26.6 pixels at the current size.
// `gposBytes`/`gposSize` is the raw content of the font's 'GPOS' table (e.g.
// from FT_Load_Sfnt_Table), not the whole font file.
//
// It walks the script, feature and lookup lists on every call; a caller that
// kerns many pairs of one font resolves them once (below).
int32_t PairKernAdjustment(const uint8_t* gposBytes, size_t gposSize, uint32_t leftGlyph, uint32_t rightGlyph);

// --- Resolved once, for many pairs ---

// The kern feature's PairPos subtables in the order they apply, as byte
// offsets from the start of the GPOS table (so they stay valid when the
// caller's copy or mapping of the table moves), kLookupStart set on the first
// subtable of each lookup.
constexpr uint32_t kLookupStart = 0x80000000u;

// Writes up to `capacity` subtables to `subtables` (null: only count) and
// returns how many the font has: more than `capacity` when they did not all
// fit. 0 when the font does not kern, or the table is too malformed to walk.
unsigned ResolveKernSubtables(const uint8_t* gposBytes, size_t gposSize, uint32_t* subtables, unsigned capacity);

// PairKernAdjustment over resolved subtables: the same value, without the
// walk. `covered`, when not null, is set to whether any subtable covers
// `leftGlyph` at all; a caller remembering pairs need not remember the ones
// whose left glyph kerns with nothing.
int32_t PairKernAdjustment(const uint8_t* gposBytes, size_t gposSize, const uint32_t* subtables, unsigned count,
                           uint32_t leftGlyph, uint32_t rightGlyph, bool* covered = nullptr);

}  // namespace gpos
}  // namespace font
}  // namespace freeink
