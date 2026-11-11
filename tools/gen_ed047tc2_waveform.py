#!/usr/bin/env python3
"""Generate the ED047TC2 LovyanGFX waveform tables from the vendor waveform blob.

Input is the epdiy-format vendor waveform header for the ED047TC2 panel used by
the LilyGo T5 S3 Pro (`epdiy_ED047TC2.h` / `ED047TC2.h` -- the two are the same
data under different symbol names).  Output is
`libs/hardware/BoardT5S3/src/ED047TC2Waveform.cpp`.

    python tools/gen_ed047tc2_waveform.py path/to/epdiy_ED047TC2.h

Vendor blob layout
------------------
Each `epd_wp_ED047TC2_<mode>_<range>_data[phases][16][4]` table holds, for one
draw mode and one temperature range, a 2-bit drive code per (destination level,
source level) pair per phase:

    code 0 = no drive,  1 = drive toward black,  2 = drive toward white

The outer index of the 16 rows is the DESTINATION level; the 4 bytes of each row
hold the 16 SOURCE levels, four per byte, most significant pair first.  Level 0
is black, level 15 is white.

Why the tables below are only two columns wide
----------------------------------------------
Decoding the blob shows every mode/range is a separable impulse waveform:

    net_frames(to, from) == L[to] - L[from]

with a zero diagonal, for one impulse vector L per temperature range shared by
all three modes (DU, GC16, GL16).  The DU tables are exactly `L[15] + 1` phases
long and drive black->white as `L[15]` consecutive white frames (and white->black
as `L[15]` consecutive black frames), so DU reduces to a single number per
temperature range: the drive length.  This script asserts all of that against the
blob before emitting anything, so a different or corrupted input fails loudly
rather than producing a plausible-looking wrong waveform.

The emitted banks turn that vector into a single-push architecture:

epd_fast/epd_fastest get the DU rails plus two SELF-NORMALIZING grey columns.
A LUT column is indexed by destination alone and cannot see where a pixel came
from, so a column that must land on a mid level from an arbitrary source first
saturates at a rail and then walks to its target: a run toward white long
enough to take any source there (the rail clamps), then k frames toward black.
One column, correct from every source state. This is what lets a page carry its
greys in the same push as its text: no separate B/W base pass exists to
pre-position the fringe, and none is needed.

k does NOT come from the impulse vector. L counts NET frames, and the vendor
lands a grey with a long mixed sequence (GC16 24..27 C, white to level 14: 15
frames toward black, then 14 toward white, then one toward black), while a grey
column here is one run toward black. Run that way, at Panel_EPD's frame rate,
this panel moves most of the way to black in the first three or four frames, so
the L[15]-L[g] frames the impulse model asks for land every grey on black. k, and
the run toward white before it, are picked from the panel's measured response
instead; see WALKDOWN_AFTER_RUN.

epd_text/epd_quality become a true GC16-style refresh: every one of the 16
columns rail-normalizes (dark half to white, light half to black) and then
walks to its exact level. Destination-indexed yet source-independent, it both
scrubs residue and resets any accumulated DC bias, because the saturating rail
visit erases a pixel's drive history.

Panel_EPD reads a pixel's progress word through a signed cast, so no block index
may reach 128: the five banks together, terminators included, must fit in 128
rows. The generator asserts that for every temperature range, because overrunning
it kills every refresh silently.

"""

import re
import sys
from pathlib import Path

BLOB_RE = re.compile(
    r"const uint8_t epd_wp_ED047TC2_(\d+)_(\d+)_data\[(\d+)\]\[16\]\[4\]\s*=\s*(.*?);",
    re.S | re.I,
)
INTERVAL_RE = re.compile(
    r"const EpdWaveformTempInterval ed047tc2_intervals\[(\d+)\]\s*=\s*(.*?);", re.S
)

MODE_DU, MODE_GC16, MODE_GL16 = 1, 2, 5

# The vendor blob carries ranges 5..11 of the 14-entry interval table.
FIRST_RANGE = 5
RANGE_COUNT = 7


def parse(path):
    text = Path(path).read_text(encoding="utf-8", errors="replace")

    tables = {}
    for m in BLOB_RE.finditer(text):
        mode, rng, phases = int(m.group(1)), int(m.group(2)), int(m.group(3))
        vals = [int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", m.group(4))]
        if len(vals) != phases * 16 * 4:
            raise SystemExit(f"mode {mode} range {rng}: expected {phases*16*4} bytes, got {len(vals)}")
        table = []
        for p in range(phases):
            rows = []
            for r in range(16):
                base = (p * 16 + r) * 4
                row = []
                for byte in vals[base : base + 4]:
                    row += [(byte >> (6 - 2 * k)) & 3 for k in range(4)]
                rows.append(row)
            table.append(rows)
        tables[(mode, rng)] = table

    im = INTERVAL_RE.search(text)
    if not im:
        raise SystemExit("no ed047tc2_intervals table found")
    nums = [int(x) for x in re.findall(r"\.(?:min|max)\s*=\s*(-?\d+)", im.group(2))]
    intervals = list(zip(nums[0::2], nums[1::2]))

    return tables, intervals


def impulse_vector(table):
    """Return L with L[0] == 0 such that net(to, from) == L[to] - L[from]."""
    net = [[0] * 16 for _ in range(16)]
    for phase in table:
        for to in range(16):
            for src in range(16):
                code = phase[to][src]
                if code == 1:
                    net[to][src] -= 1
                elif code == 2:
                    net[to][src] += 1
                elif code == 3:
                    raise SystemExit("unexpected drive code 3 in vendor blob")
    return net, [net[to][0] - net[0][0] for to in range(16)]


def check(tables, intervals):
    """Validate every structural assumption the generated tables rely on."""
    ranges = list(range(FIRST_RANGE, FIRST_RANGE + RANGE_COUNT))
    for mode in (MODE_DU, MODE_GC16, MODE_GL16):
        for rng in ranges:
            if (mode, rng) not in tables:
                raise SystemExit(f"vendor blob is missing mode {mode} range {rng}")
    if len(intervals) < FIRST_RANGE + RANGE_COUNT:
        raise SystemExit("interval table too short for ranges 5..11")

    impulses = {}
    for rng in ranges:
        net_gc, l_gc = impulse_vector(tables[(MODE_GC16, rng)])
        # GC16 must be exactly separable with a zero diagonal.
        for to in range(16):
            for src in range(16):
                if net_gc[to][src] != l_gc[to] - l_gc[src]:
                    raise SystemExit(f"range {rng}: GC16 is not separable at ({to},{src})")
            if net_gc[to][to] != 0:
                raise SystemExit(f"range {rng}: GC16 diagonal is not zero at {to}")
        if l_gc != sorted(l_gc):
            raise SystemExit(f"range {rng}: GC16 impulse vector is not monotonic: {l_gc}")

        # GL16 must agree with GC16 on the impulse vector.
        _, l_gl = impulse_vector(tables[(MODE_GL16, rng)])
        if l_gl != l_gc:
            raise SystemExit(f"range {rng}: GL16 impulse {l_gl} != GC16 impulse {l_gc}")

        # DU must be the two-level restriction of the same vector, run as one
        # contiguous burst starting at phase 0, and nothing else.
        du = tables[(MODE_DU, rng)]
        drive = l_gc[15]
        if len(du) != drive + 1:
            raise SystemExit(f"range {rng}: DU has {len(du)} phases, expected {drive + 1}")
        for to in range(16):
            for src in range(16):
                seq = [phase[to][src] for phase in du]
                if to == 15 and src == 0:
                    want = [2] * drive + [0]
                elif to == 0 and src == 15:
                    want = [1] * drive + [0]
                elif to in (0, 15):
                    continue  # intermediate sources: unused by a binarised canvas
                else:
                    want = [0] * len(du)
                if seq != want:
                    raise SystemExit(f"range {rng}: DU[{to}][{src}] = {seq}, expected {want}")
        impulses[rng] = l_gc
    return impulses


# How the panel answers one run of drive, measured rather than derived: a LilyGo
# T5 S3 Pro at 24 C (range 24..27 C, so L[15] = 21), 100x100 px patches through
# the fast bank, a 4K camera, each patch against the black patch and the white
# background of the same frame (lightness 0 = black, 1 = white). Means of a white
# and a black start, two pushes each.
#
# Other ranges are scaled by their drive length: n frames at L[15] = D land where
# n * WALKDOWN_DRIVE / D frames did here. That is the vendor's own temperature
# compensation (a colder panel gets a longer DU drive for the same transition),
# assumed to hold for a partial drive too; only 24..27 C was measured.
WALKDOWN_DRIVE = 21

# m frames toward white after L[15] toward black, m = 0, 1, 2, ...: a black pixel
# is white after GRAY_WHITE_RUN frames, well inside L[15]. (The dip at m = 8 is
# within the 0.02 of camera noise check_response() allows.)
WALKUP_LIGHTNESS = [0.00, 0.08, 0.42, 0.74, 0.87, 0.93, 0.96, 0.97, 0.95, 0.96, 0.98, 0.99, 1.00]
GRAY_WHITE_RUN = 12

# k frames toward black after GRAY_WHITE_RUN toward white: where a grey column
# lands. A white and a black start agree within 0.05. From k = 10 on it is black.
WALKDOWN_AFTER_RUN = [1.00, 0.85, 0.59, 0.37, 0.20, 0.12, 0.07, 0.09, 0.04, 0.01, 0.00]

# The same after the full L[15] toward white: where a clean-bank column lands (its
# last phase walks L[15]-L[g] frames down from the white rail). At 24 C the clean
# bank's level 13 (5 frames) read 0.10 and level 14 (2 frames) 0.55 on their own.
WALKDOWN_AFTER_RAIL = [1.00, 0.82, 0.56, 0.34, 0.19, 0.11, 0.07, 0.08, 0.04, 0.01, 0.00]

# Optical targets for the two AA greys, as lightness on the scale above. The font
# buckets a glyph edge at 25-50% ink (its light grey) and 50-75% ink (its dark
# grey); these are those buckets' midpoints, so the panel reproduces the coverage
# the font quantiser measured.
#
# Do not reach for these to make text heavier or lighter -- that is what the
# reader's stroke weight setting is for, and it works by remapping which bucket
# gets which tone rather than by moving the tones themselves. Retune these only
# against a grey that reads as the wrong *shade* next to its neighbours.
GRAY_TARGET_DARK = 0.375
GRAY_TARGET_LIGHT = 0.625

# A grey is only usable if it stays clear of white, clear of black and clear of
# the other grey.
GRAY_MAX_LIGHTNESS = 0.95
GRAY_MIN_LIGHTNESS = 0.05
GRAY_MIN_SEPARATION = 0.15

# All five banks (eraser + quality + text + fast + fastest) are laid out
# consecutively in one expanded table, and NO BLOCK INDEX MAY EVER REACH 128.
#
# Panel_EPD keeps a pixel's refresh progress in a uint16_t as
# (lut_block << 8) | level, and blit_dmabuf reads it back through a SIGNED cast
# and skips the pixel when the result is negative -- bit 15 means "this pixel is
# idle". A pixel advances one block per frame until it reads a zero LUT entry, so
# the terminator of the LAST bank is the highest index anything reaches: the
# total row count, terminators included, must be <= 128.
#
# Overrunning it fails SILENTLY and GLOBALLY -- every pixel whose waveform crosses
# block 128 reads as idle mid-refresh, so refreshes stop completing and the panel
# looks dead. Because a colder range needs longer banks, it can present as a board
# that works warm and blanks cool. That is what happened here once, at fast start
# block 131.
#
# 255 is NOT the limit, though the uint8_t _lut_offset_table invites that reading:
# that bounds the offset TYPE, while the signed progress read is what binds.
LUT_ROW_BUDGET = 128

# LovyanGFX lut_eraser, counted from Panel_EPD.cpp: two drive rows, a ~0u no-op
# row, and the 0u terminator. Four, not three -- undercounting it puts every
# total and every bank offset one row below the truth, in the unsafe direction.
ERASER_ROWS = 4
UNUSED_SLOT_ROWS = 1  # the board config stubs epd_quality/epd_fastest (see there)


def check_response():
    """The measured responses must run rail to rail, and only one way.

    A step back of up to 0.02 is camera noise (k = 7 reads 0.01 above k = 6 after
    the rail) and is levelled by lightness(); anything larger means a table is not
    a one-way response and nothing picked from it can be trusted.
    """
    for name, table, sign in (("WALKDOWN_AFTER_RUN", WALKDOWN_AFTER_RUN, -1),
                              ("WALKDOWN_AFTER_RAIL", WALKDOWN_AFTER_RAIL, -1),
                              ("WALKUP_LIGHTNESS", WALKUP_LIGHTNESS, 1)):
        first, last = (1.0, 0.0) if sign < 0 else (0.0, 1.0)
        if table[0] != first or abs(table[-1] - last) > GRAY_MIN_LIGHTNESS:
            raise SystemExit("%s must run from %.0f to %.0f: %s" % (name, first, last, table))
        for k in range(1, len(table)):
            if (table[k] - table[k - 1]) * sign < -0.02 - 1e-9:
                raise SystemExit("%s turns back at %d: %s" % (name, k, table))
    if WALKUP_LIGHTNESS[min(GRAY_WHITE_RUN, len(WALKUP_LIGHTNESS) - 1)] < 1.0 - 0.01:
        raise SystemExit("GRAY_WHITE_RUN = %d does not reach white" % GRAY_WHITE_RUN)


def lightness(table, frames, drive):
    """Lightness after `frames` of one run, read off a walk-down `table`, in a
    range with a DU drive of `drive` frames."""
    levelled = []
    for v in table:
        levelled.append(min(v, levelled[-1]) if levelled else v)  # level the noise
    x = frames * WALKDOWN_DRIVE / drive
    if x >= len(levelled) - 1:
        return levelled[-1]
    i = int(x)
    return levelled[i] + (levelled[i + 1] - levelled[i]) * (x - i)


def white_run(drive):
    """Frames toward white a grey column starts with in a range of `drive`
    frames: GRAY_WHITE_RUN scaled, rounded up."""
    return -(-GRAY_WHITE_RUN * drive // WALKDOWN_DRIVE)


def pick_gray_frames(drive):
    """Pick the walk-down lengths (k_dark, k_light) of the two grey columns.

    Each is the length that lands nearest its target in this range, the dark one
    strictly longer than the light one. Ties go to the shorter walk, which is the
    shorter waveform.
    """
    def land(k):
        return lightness(WALKDOWN_AFTER_RUN, k, drive)

    def nearest(target, shortest):
        return min(range(shortest, drive + 1), key=lambda k: (abs(land(k) - target), k))

    k_light = nearest(GRAY_TARGET_LIGHT, 1)
    k_dark = nearest(GRAY_TARGET_DARK, k_light + 1)
    light, dark = land(k_light), land(k_dark)
    if light > GRAY_MAX_LIGHTNESS or dark < GRAY_MIN_LIGHTNESS or light - dark < GRAY_MIN_SEPARATION:
        raise SystemExit(
            "drive %d: greys at k=%d/%d land at %.2f/%.2f, not two usable greys" % (drive, k_dark, k_light, dark, light)
        )
    return k_dark, k_light


def pick_gray_levels(impulse, k_dark, k_light):
    """Pick the (dark, light) canvas levels that carry the two grey columns.

    In the fast bank any level could carry them: the column is cut for it. The
    choice matters for the OTHER bank. A Half or Full refresh pushes the same
    canvas through the clean bank, whose column for level g walks L[15]-L[g]
    frames down from the white rail, and that lands where WALKDOWN_AFTER_RAIL
    says. So the pair is the one whose clean landings come nearest the fast bank's
    greys, the dark level below the light one, and the periodic clean page shows
    the same text as the page turns as far as the vendor columns allow. Ties go to
    the lower levels.
    """
    drive = impulse[15]
    clean = [lightness(WALKDOWN_AFTER_RAIL, drive - impulse[lv], drive) for lv in range(16)]
    want_dark = lightness(WALKDOWN_AFTER_RUN, k_dark, drive)
    want_light = lightness(WALKDOWN_AFTER_RUN, k_light, drive)

    best = None
    for dark in range(1, 15):
        for light in range(dark + 1, 15):
            err = (clean[dark] - want_dark) ** 2 + (clean[light] - want_light) ** 2
            key = (round(err, 9), dark, light)
            if best is None or key < best:
                best = key
    return best[1], best[2]


def fast_rows(l15, run, dark, k_dark, light, k_light):
    """The differential bank: DU rails plus two self-normalizing grey columns.

    Rails run the vendor DU verbatim: L[15] frames of full drive, then park.
    Each grey column first drives `run` frames toward white, which takes any
    source to white -- erasing whatever state the pixel arrived in, the rail clamp
    doing the work a source index would otherwise have to -- and then walks back
    down toward black for its k frames to land on its grey. Not the rails' L[15]:
    every frame past white is drive the pixel never gets back. Measured at 24 C,
    100 px grey patches over four page turns and then a turn to white: with a run
    of L[15] they stayed ~5% darker than the white around them, with a run of 12
    ~1.5% (the old near-black greys ~0.6%). The bank is as long as the longer of
    the two, which is L[15] in every range.
    """
    rows = []
    for f in range(max(l15, run + max(k_dark, k_light))):
        codes = [3] * 16
        if f < l15:
            codes[0] = 1
            codes[15] = 2
        for level, k in ((dark, k_dark), (light, k_light)):
            if f < run:
                codes[level] = 2
            elif f < run + k:
                codes[level] = 1
        rows.append("    LUT_MAKE(%s)," % ", ".join(str(c) for c in codes))
    return rows


def clean_rows(impulse):
    """The GC16-style clean bank for epd_text / epd_quality.

    Three phases, uniform across all 16 columns so the refresh reads as a
    blink rather than as noise: every pixel drives to the black rail for L[15]
    frames, then to the white rail for L[15], then walks down from white to its
    exact level (L[15]-L[i] frames toward black). The double rail excursion is
    the scrub -- it erases drive history and accumulated DC bias -- and the
    final descent lands every level, greys included, precisely.

    An earlier cut of this bank sent each pixel to the rail OPPOSITE its
    destination and back, which scrubbed and landed just as well but showed the
    old page fading THROUGH the new page inverted, both at once: on a real
    device the cadence refresh read as a screenful of garbage before the text
    resolved. Same physics, reordered for the eye watching it.
    """
    l15 = impulse[15]
    rows = []
    for _ in range(l15):
        rows.append("    LUT_MAKE(%s)," % ", ".join(["1"] * 16))
    for _ in range(l15):
        rows.append("    LUT_MAKE(%s)," % ", ".join(["2"] * 16))
    for f in range(l15):
        codes = ["1" if f < l15 - impulse[i] else "3" for i in range(16)]
        rows.append("    LUT_MAKE(%s)," % ", ".join(codes))
    return rows


HEADER = '''// GENERATED FILE -- DO NOT EDIT BY HAND.
//
// Regenerate with:
//     python tools/gen_ed047tc2_waveform.py <vendor ED047TC2 waveform header>
//
// Source: the ED047TC2 vendor waveform in epdiy form -- LilyGo ships it as
// Waveform_header/ED047TC2.h in Xinyuan-LilyGO/LilyGo-EPD47, epdiy as
// epdiy_ED047TC2.h; the two are the same data under different symbol names.
// See tools/gen_ed047tc2_waveform.py for the blob layout and for the structural
// checks the generator runs against it.
//
// The vendor waveform is a separable impulse waveform: a transition from source
// level `f` to destination level `t` needs `L[t] - L[f]` frames of drive, toward
// white when positive and toward black when negative, where L is the per
// temperature range impulse vector below (level 0 is black, 15 is white):
//
{impulse_comment}
//
// Both emitted banks are SELF-NORMALIZING: a LovyanGFX LUT column is indexed by
// destination alone, so any column that must land on a mid level from an
// arbitrary source first saturates at a rail -- the clamp erases the pixel\'s
// history -- and then walks to its target. That is what lets a page carry its
// anti-aliasing greys in the same push as its text, with no separate B/W base
// pass to pre-position the fringe.
//
// kFastLut (epd_fast / epd_fastest): the vendor DU rails verbatim, plus a grey
// column per AA tone that drives toward white long enough to take any source
// there and then descends k frames toward black. Both lengths come from the
// panel\'s measured response (WALKUP_LIGHTNESS, WALKDOWN_AFTER_RUN in the
// generator), not from L: run as one burst at this driver\'s frame rate,
// L[15]-L[g] frames land every grey on black. Panel_EPD\'s per-pixel diff means
// a pixel is only driven when its target changes, so a fringe pixel that stays
// the same grey across a page turn is not driven at all -- which is also the
// charge story: drives happen on content changes only, and each grey drive
// begins with a saturating visit to the white rail.
//
// kCleanLut (epd_text / epd_quality): a GC16-style refresh. Every one of the 16
// columns rail-normalizes (dark half to white, light half to black) and then
// walks to its exact level, so the periodic clean page both scrubs residue and
// re-lands every grey precisely. The previous bank drove each level away and
// symmetrically back, which is only a correct landing for the two rails; any
// grey pushed through it ended on a rail.
//
// The greys are decided per temperature range: the run up and the walk-down k
// from the measured response scaled by the range\'s drive length, and the canvas
// level that carries each one as the level whose clean-bank column lands nearest
// it, so a Half or Full refresh shows the same greys as far as the vendor columns
// allow (at 33..38 C levels 12 through 15 are all the same optical white there).
// The board config reads kGrayLevelDark/kGrayLevelLight for the range it selected
// the LUT for and writes the matching canvas bytes, so the two cannot drift apart.
// Frames up toward white, k frames down toward black, and the lightness the
// measured response predicts for each grey in the fast bank and for its level in
// the clean bank (0 = black, 1 = white):
//
{gray_comment}
//
// Keeping the drive length matched to the panel temperature IS the temperature
// compensation: e-ink particles move more slowly when cold, so a cold panel
// needs a longer push for the same optical result.
//
// Panel_EPD reads a pixel's refresh progress through a signed cast, so no block
// index may reach 128: all five banks together (eraser + quality + text + fast +
// fastest), terminators included, must fit in 128 rows. Overrunning that stops
// every refresh completing, with no error. Worst case here: {worst_rows} rows.

#include <ED047TC2Waveform.h>

namespace freeink {{
namespace ed047tc2 {{

namespace {{

// One frame of a LovyanGFX Panel_EPD LUT: sixteen 2-bit drive codes, indexed by
// the destination level. 0 ends the sequence, 1 drives toward black, 2 drives
// toward white, 3 is a no-op that keeps the sequence running.
#define LUT_MAKE(d0, d1, d2, d3, d4, d5, d6, d7, d8, d9, da, db, dc, dd, de, df)         \\
  (uint32_t)((d0 << 0) | (d1 << 2) | (d2 << 4) | (d3 << 6) | (d4 << 8) | (d5 << 10) |    \\
             (d6 << 12) | (d7 << 14) | (d8 << 16) | (d9 << 18) | (da << 20) |            \\
             (db << 22) | (dc << 24) | (dd << 26) | (de << 28) | (df << 30))

'''

FOOTER = '''#undef LUT_MAKE

}}  // namespace

const TempRange kTempRanges[kTempRangeCount] = {{
{ranges}
}};

// The canvas levels the AA grey columns are cut for, per temperature range. A
// board config turns these into the grey bytes its canvas writes; they are not
// independently tunable -- retarget them in tools/gen_ed047tc2_waveform.py and
// regenerate, or the canvas will address a column the LUT does not drive.
const uint8_t kGrayLevelDark[kTempRangeCount] = {{{gray_dark_levels}}};

const uint8_t kGrayLevelLight[kTempRangeCount] = {{{gray_light_levels}}};

// Each grey column drives kGrayWhiteFrames toward white, which takes any source
// to white, then its kGrayFrames* toward black, per temperature range.
const uint8_t kGrayWhiteFrames[kTempRangeCount] = {{{gray_white_frames}}};

const uint8_t kGrayFramesDark[kTempRangeCount] = {{{gray_dark_frames}}};

const uint8_t kGrayFramesLight[kTempRangeCount] = {{{gray_light_frames}}};

const uint32_t* const kFastLut[kTempRangeCount] = {{
{fast_ptrs}
}};

const size_t kFastLutStep[kTempRangeCount] = {{
{fast_steps}
}};

const uint32_t* const kCleanLut[kTempRangeCount] = {{
{clean_ptrs}
}};

const size_t kCleanLutStep[kTempRangeCount] = {{
{clean_steps}
}};

const uint8_t kDriveFrames[kTempRangeCount] = {{{drive_frames}}};

size_t tempRangeIndex(int tempC) {{
  for (size_t i = 0; i < kTempRangeCount; ++i) {{
    if (tempC < kTempRanges[i].maxC) return i;
  }}
  return kTempRangeCount - 1;
}}

}}  // namespace ed047tc2
}}  // namespace freeink
'''


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    tables, intervals = parse(sys.argv[1])
    impulses = check(tables, intervals)
    check_response()

    ranges = list(range(FIRST_RANGE, FIRST_RANGE + RANGE_COUNT))
    drives = [impulses[r][15] for r in ranges]
    frames = {r: pick_gray_frames(impulses[r][15]) for r in ranges}
    runs = {r: white_run(impulses[r][15]) for r in ranges}
    grays = {r: pick_gray_levels(impulses[r], *frames[r]) for r in ranges}

    impulse_comment = "\n".join(
        "//   {:>2}..{:<2} C  L = [{}]".format(
            intervals[r][0], intervals[r][1], ", ".join("%2d" % v for v in impulses[r])
        )
        for r in ranges
    )
    def landing(r, k, level):
        drive = impulses[r][15]
        return (lightness(WALKDOWN_AFTER_RUN, k, drive),
                lightness(WALKDOWN_AFTER_RAIL, drive - impulses[r][level], drive))

    gray_comment = "\n".join(
        "//   {:>2}..{:<2} C  up {:>2}  dark = level {:>2}, k={} ({:.2f}; clean {:.2f})"
        "   light = level {:>2}, k={} ({:.2f}; clean {:.2f})".format(
            intervals[r][0],
            intervals[r][1],
            runs[r],
            grays[r][0],
            frames[r][0],
            *landing(r, frames[r][0], grays[r][0]),
            grays[r][1],
            frames[r][1],
            *landing(r, frames[r][1], grays[r][1]),
        )
        for r in ranges
    )

    # One bank pair per distinct (vector, grey pair); dedup by content.
    fast_banks = {}   # rows-tuple -> name
    clean_banks = {}
    fast_of = {}
    clean_of = {}
    worst_rows = 0
    for idx, r in enumerate(ranges):
        L = impulses[r]
        d, l = grays[r]
        fr = tuple(fast_rows(L[15], runs[r], d, frames[r][0], l, frames[r][1]))
        cr = tuple(clean_rows(L))
        fast_of[r] = fast_banks.setdefault(fr, "kFastR%d" % idx)
        clean_of[r] = clean_banks.setdefault(cr, "kCleanR%d" % idx)
        # +1 per bank for the terminator row Panel_EPD copies too. Quality and
        # fastest are stubbed in the board config, so they cost one row each.
        total = ERASER_ROWS + 2 * UNUSED_SLOT_ROWS + (len(cr) + 1) + (len(fr) + 1)
        worst_rows = max(worst_rows, total)
        # One check, on the total, because the banks are consecutive: the last
        # bank's terminator sits at block total-1 and is the highest index any
        # pixel reaches, so bounding the total bounds every bank at once. (A
        # separate assertion on where the fast bank STARTS would be redundant --
        # it cannot exceed a total that is already in range.)
        if total > LUT_ROW_BUDGET:
            raise SystemExit(
                "range %d: %d LUT rows, %d over budget. Panel_EPD reads a pixel's"
                " progress word as signed, so block %d would read as idle and"
                " refreshes would stop completing -- the panel goes dead with no"
                " error. Shorten a bank."
                % (r, total, total - LUT_ROW_BUDGET, total - 1)
            )

    body = HEADER.format(
        impulse_comment=impulse_comment, gray_comment=gray_comment, worst_rows=worst_rows
    )
    for rows, name in fast_banks.items():
        body += "// Differential bank: DU rails + self-normalizing AA grey columns.\n"
        body += "constexpr uint32_t %s[] = {\n%s\n    0u,\n};\n\n" % (name, "\n".join(rows))
    for rows, name in clean_banks.items():
        body += "// GC16-style clean: every level rail-normalizes, then lands exactly.\n"
        body += "constexpr uint32_t %s[] = {\n%s\n    0u,\n};\n\n" % (name, "\n".join(rows))

    body += FOOTER.format(
        ranges="\n".join(
            "    {%d, %d}," % (intervals[r][0], intervals[r][1]) for r in ranges
        ),
        gray_dark_levels=", ".join(str(grays[r][0]) for r in ranges),
        gray_light_levels=", ".join(str(grays[r][1]) for r in ranges),
        gray_white_frames=", ".join(str(runs[r]) for r in ranges),
        gray_dark_frames=", ".join(str(frames[r][0]) for r in ranges),
        gray_light_frames=", ".join(str(frames[r][1]) for r in ranges),
        fast_ptrs="\n".join("    %s," % fast_of[r] for r in ranges),
        fast_steps="\n".join(
            "    sizeof(%s) / sizeof(%s[0])," % (fast_of[r], fast_of[r]) for r in ranges
        ),
        clean_ptrs="\n".join("    %s," % clean_of[r] for r in ranges),
        clean_steps="\n".join(
            "    sizeof(%s) / sizeof(%s[0])," % (clean_of[r], clean_of[r]) for r in ranges
        ),
        drive_frames=", ".join(str(d) for d in drives),
    )

    out = (
        Path(__file__).resolve().parent.parent
        / "libs/hardware/BoardT5S3/src/ED047TC2Waveform.cpp"
    )
    out.write_text(body, encoding="utf-8")
    print("wrote", out)
    for idx, r in enumerate(ranges):
        L = impulses[r]
        d, l = grays[r]
        print(
            "  range %2d  %2d..%-2d C  drive=%2d  fast=%s(%d rows)  clean=%s(%d rows)"
            "  greys=lvl %d/%d, up %d down %d/%d"
            % (
                r, intervals[r][0], intervals[r][1], L[15],
                fast_of[r], len(fast_rows(L[15], runs[r], d, frames[r][0], l, frames[r][1])),
                clean_of[r], len(clean_rows(L)), d, l, runs[r], frames[r][0], frames[r][1],
            )
        )


if __name__ == "__main__":
    main()
