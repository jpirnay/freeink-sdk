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
// The lookups are those of the 'kern' feature of every script's
// DefaultLangSys (its required feature included), taken together, since this
// API has no script input. A shaper applies only the run's script's kern
// lookups, and falls back to the legacy 'kern' table (KernTable.h) when that
// script has none. The two agree when the font points every script at the
// same kern lookups, as most fonts do (Latin, Cyrillic and Greek then kern
// alike); a font that splits its kern lookups by script gets their union
// here. Named language systems are not consulted either: there is no
// language input. The lookups apply as a shaper applies them: in LookupList
// order, the first subtable that matches the pair in each, their adjustments
// added.
//
// Only the FIRST value record's X-advance adjustment is returned (how much
// the left glyph's advance changes before drawing the right glyph) — that is
// the entirety of horizontal pair kerning. Placement adjustments, second-
// glyph records, and mark/cursive attachment are out of scope.
//
// A lookup's LookupFlag can tell it to skip glyphs by their class in the
// font's GDEF table: base glyphs, ligatures, marks, or the marks outside one
// mark attachment class or one mark glyph set. Given the GDEF table, these
// readers apply those flags to the pair's two glyphs as HarfBuzz does: a
// lookup that skips either glyph does not kern the pair. A shaper would also
// look past a skipped glyph to the next one (kern 'A' with 'V' across a mark
// between them); with adjacent pairs only, these readers cannot. Without GDEF
// (null, or one whose header they cannot read) every glyph is unclassified and
// no lookup skips it, which is how the overloads without GDEF read every font.
// RightToLeft and the reserved flag bits change nothing for a pair adjustment.

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
// `gdefBytes`/`gdefSize` is the font's 'GDEF' table, whose glyph classes the
// lookup flags read (null when the font has none).
//
// It walks the script, feature and lookup lists on every call; a caller that
// kerns many pairs of one font resolves them once (below).
int32_t PairKernAdjustment(const uint8_t* gposBytes, size_t gposSize, uint32_t leftGlyph, uint32_t rightGlyph,
                           const uint8_t* gdefBytes, size_t gdefSize);
// The same without GDEF: every glyph unclassified.
int32_t PairKernAdjustment(const uint8_t* gposBytes, size_t gposSize, uint32_t leftGlyph, uint32_t rightGlyph);

// --- Resolved once, for many pairs ---

// The kern feature's lookups in the order they apply, each followed by its
// PairPos subtables, as byte offsets from the start of the GPOS table (so they
// stay valid when the caller's copy or mapping of the table moves). A lookup's
// entry has kLookupStart set: the resolved overload reads its LookupFlag
// there, and its subtables' matches start afresh.
constexpr uint32_t kLookupStart = 0x80000000u;

// Writes up to `capacity` entries (lookups and subtables) to `subtables`
// (null: only count) and returns how many the font has: more than `capacity`
// when they did not all fit. 0 when the font does not kern, or the table is
// too malformed to walk. A lookup with no PairPos subtable has no entry.
unsigned ResolveKernSubtables(const uint8_t* gposBytes, size_t gposSize, uint32_t* subtables, unsigned capacity);

// PairKernAdjustment over resolved entries, without the walk. It gives the
// per-call overload's value for the same GDEF when two things hold: that walk
// stays within its budget (kQueryBudget reads per call, where resolving
// allows kResolveBudget; both in Gpos.cpp), and the caller passes every entry
// ResolveKernSubtables() found. A table too large for the per-call walk can
// kern differently here, as the resolved entries reach further; a caller
// that keeps only the first ones kerns with those alone. This overload has no
// budget of its own: it reads each of the `count` entries it is given.
// `covered`, when not null, is set to whether any subtable of a lookup that
// does not skip `leftGlyph` covers it, whatever the right glyph (a lookup's
// flags skip a glyph by the glyph's class alone); it is false when either
// glyph is 0 (the value is then 0). A caller remembering pairs need not
// remember the ones whose left glyph kerns with nothing.
int32_t PairKernAdjustment(const uint8_t* gposBytes, size_t gposSize, const uint32_t* subtables, unsigned count,
                           uint32_t leftGlyph, uint32_t rightGlyph, const uint8_t* gdefBytes, size_t gdefSize,
                           bool* covered = nullptr);
// The same without GDEF: every glyph unclassified.
int32_t PairKernAdjustment(const uint8_t* gposBytes, size_t gposSize, const uint32_t* subtables, unsigned count,
                           uint32_t leftGlyph, uint32_t rightGlyph, bool* covered = nullptr);

}  // namespace gpos
}  // namespace font
}  // namespace freeink
