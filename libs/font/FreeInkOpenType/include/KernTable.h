#pragma once

// FreeInk SDK — the legacy TrueType 'kern' table, for fonts that have no GPOS
// kerning (see Gpos.h). Reads the Microsoft table (version 0), its horizontal
// format 0 subtables that hold kerning values (not minimums), added together
// unless a subtable says it overrides: what FreeType's FT_Get_Kerning() gives
// for such a table, with two exceptions. Cross-stream subtables, which
// FreeType adds to the horizontal value and this reader skips (they move
// glyphs across the line, not along it), and unsorted subtables, which
// FreeType searches linearly and this reader's binary searches can miss.
// Bounds-checked like the layout readers.

#include <stddef.h>
#include <stdint.h>

namespace freeink {
namespace font {
namespace kerntable {

// The kerning of `rightGlyph` after `leftGlyph` in FONT UNITS, or 0. `kernBytes`/`kernSize` is the raw content of the
// font's 'kern' table. `covered`, when not null, is set to whether any subtable kerns `leftGlyph` with anything.
int32_t PairKernAdjustment(const uint8_t* kernBytes, size_t kernSize, uint32_t leftGlyph, uint32_t rightGlyph,
                           bool* covered = nullptr);

}  // namespace kerntable
}  // namespace font
}  // namespace freeink
