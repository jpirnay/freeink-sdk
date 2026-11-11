#!/usr/bin/env python3
"""Checks for tools/gen_ed047tc2_waveform.py and the ED047TC2Waveform.cpp it wrote.

    python3 tools/test_gen_ed047tc2_waveform.py

Needs no vendor blob: the impulse vectors are read back from the generated file's
own header, so these tests also prove that the committed tables are what the
generator makes of them (grey levels, run lengths, fast banks, budget).
"""

import importlib.util
import re
import sys
import unittest
from pathlib import Path

sys.dont_write_bytecode = True  # loading the generator must not leave tools/__pycache__ behind

TOOLS = Path(__file__).resolve().parent
GENERATED = TOOLS.parent / "libs/hardware/BoardT5S3/src/ED047TC2Waveform.cpp"

_spec = importlib.util.spec_from_file_location("gen_ed047tc2_waveform", TOOLS / "gen_ed047tc2_waveform.py")
gen = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(gen)


def read_generated():
    """The generated file, taken apart: impulses, per-range arrays and banks."""
    text = GENERATED.read_text(encoding="utf-8")
    impulses = [
        [int(v) for v in m.group(1).split(",")]
        for m in re.finditer(r"^//\s+\d+\.\.\d+\s+C\s+L = \[([^\]]*)\]", text, re.M)
    ]

    def array(name):
        m = re.search(r"const uint8_t %s\[kTempRangeCount\] = \{([^}]*)\};" % name, text)
        return [int(v) for v in m.group(1).split(",")]

    def pointers(name):
        m = re.search(r"const uint32_t\* const %s\[kTempRangeCount\] = \{([^}]*)\};" % name, text)
        return [v.strip() for v in m.group(1).split(",") if v.strip()]

    banks = {}
    for m in re.finditer(r"constexpr uint32_t (\w+)\[\] = \{\n(.*?)\n    0u,\n\};", text, re.S):
        banks[m.group(1)] = [
            [int(c) for c in row.split(",")] for row in re.findall(r"LUT_MAKE\(([^)]*)\)", m.group(2))
        ]
    return {
        "impulses": impulses,
        "dark": array("kGrayLevelDark"),
        "white_run": array("kGrayWhiteFrames"),
        "light": array("kGrayLevelLight"),
        "frames_dark": array("kGrayFramesDark"),
        "frames_light": array("kGrayFramesLight"),
        "drive": array("kDriveFrames"),
        "fast": [banks[n] for n in pointers("kFastLut")],
        "clean": [banks[n] for n in pointers("kCleanLut")],
    }


def land(bank, level, start, rail):
    """Where a pixel starting `start` frames above black ends after `bank`'s column
    for `level`, clamped to the rails as the panel clamps (LgfxEpdDriver.cpp,
    simulateGrayColumn)."""
    pos = start
    for row in bank:
        code = row[level]
        pos += 1 if code == 2 else -1 if code == 1 else 0
        pos = max(0, min(rail, pos))
    return pos


class ResponseTables(unittest.TestCase):
    def test_tables_run_one_way(self):
        gen.check_response()  # raises SystemExit if not

    def test_levelled_and_scaled(self):
        rail = gen.WALKDOWN_AFTER_RAIL
        self.assertEqual(gen.lightness(rail, 0, 21), 1.0)
        self.assertAlmostEqual(gen.lightness(rail, 2, 21), 0.56)
        # The noisy step at k = 7 is levelled, never a rise.
        self.assertLessEqual(gen.lightness(rail, 7, 21), gen.lightness(rail, 6, 21))
        # Twice the drive length, twice the frames for the same landing.
        self.assertAlmostEqual(gen.lightness(rail, 4, 42), gen.lightness(rail, 2, 21))
        self.assertEqual(gen.lightness(rail, 40, 21), rail[-1])

    def test_white_run(self):
        self.assertEqual(gen.white_run(21), gen.GRAY_WHITE_RUN)
        self.assertEqual(gen.white_run(14), 8)  # 12 * 14 / 21, rounded up
        for drive in range(1, 64):
            self.assertLessEqual(gen.white_run(drive), drive)  # inside the rails


class GreyChoice(unittest.TestCase):
    def test_measured_range_lands_near_the_targets(self):
        # 24..27 C is where the response was measured: 3 and 2 frames land at
        # 0.34 and 0.56, against targets of 0.375 and 0.625.
        self.assertEqual(gen.pick_gray_frames(21), (3, 2))

    def test_every_range_has_two_distinct_greys(self):
        for impulse in read_generated()["impulses"]:
            drive = impulse[15]
            k_dark, k_light = gen.pick_gray_frames(drive)
            dark = gen.lightness(gen.WALKDOWN_AFTER_RUN, k_dark, drive)
            light = gen.lightness(gen.WALKDOWN_AFTER_RUN, k_light, drive)
            self.assertGreater(k_dark, k_light)
            self.assertGreaterEqual(k_light, 1)
            self.assertGreaterEqual(light - dark, gen.GRAY_MIN_SEPARATION)
            self.assertLessEqual(light, gen.GRAY_MAX_LIGHTNESS)
            self.assertGreaterEqual(dark, gen.GRAY_MIN_LIGHTNESS)

    def test_levels_follow_the_clean_bank(self):
        # 24..27 C: the clean columns of levels 14 and 13 walk 2 and 5 frames down
        # from white, the only two that are not black; the pair takes them.
        impulse = [0, 3, 3, 5, 6, 6, 7, 8, 9, 9, 9, 9, 12, 16, 19, 21]
        self.assertEqual(gen.pick_gray_levels(impulse, *gen.pick_gray_frames(21)), (13, 14))
        for impulse in read_generated()["impulses"]:
            dark, light = gen.pick_gray_levels(impulse, *gen.pick_gray_frames(impulse[15]))
            self.assertTrue(1 <= dark < light <= 14, (dark, light))

    def test_impossible_targets_fail_loudly(self):
        saved = gen.GRAY_MIN_SEPARATION
        gen.GRAY_MIN_SEPARATION = 0.9
        try:
            with self.assertRaises(SystemExit):
                gen.pick_gray_frames(21)
        finally:
            gen.GRAY_MIN_SEPARATION = saved


class FastBank(unittest.TestCase):
    @staticmethod
    def codes(rows):
        return [[int(c) for c in re.search(r"LUT_MAKE\(([^)]*)\)", r).group(1).split(",")] for r in rows]

    def test_columns(self):
        rows = self.codes(gen.fast_rows(21, 12, 13, 3, 14, 2))
        self.assertEqual(len(rows), 21)  # the rails; the greys end inside them
        for f, row in enumerate(rows):
            self.assertEqual(row[0], 1)
            self.assertEqual(row[15], 2)
            self.assertEqual(row[13], 2 if f < 12 else (1 if f < 15 else 3))
            self.assertEqual(row[14], 2 if f < 12 else (1 if f < 14 else 3))
            for level in range(1, 13):
                self.assertEqual(row[level], 3)

    def test_greys_past_the_rails_lengthen_the_bank(self):
        rows = self.codes(gen.fast_rows(4, 4, 13, 3, 14, 2))
        self.assertEqual(len(rows), 7)
        self.assertEqual([r[0] for r in rows], [1, 1, 1, 1, 3, 3, 3])
        self.assertEqual([r[13] for r in rows], [2, 2, 2, 2, 1, 1, 1])


class GeneratedFile(unittest.TestCase):
    """The committed ED047TC2Waveform.cpp is what the generator makes."""

    @classmethod
    def setUpClass(cls):
        cls.g = read_generated()

    def test_seven_ranges(self):
        for key in ("impulses", "dark", "light", "white_run", "frames_dark", "frames_light", "drive", "fast", "clean"):
            self.assertEqual(len(self.g[key]), 7, key)

    def test_arrays_match_the_generator(self):
        for i, impulse in enumerate(self.g["impulses"]):
            frames = gen.pick_gray_frames(impulse[15])
            self.assertEqual((self.g["frames_dark"][i], self.g["frames_light"][i]), frames)
            self.assertEqual(self.g["white_run"][i], gen.white_run(impulse[15]))
            self.assertEqual((self.g["dark"][i], self.g["light"][i]), gen.pick_gray_levels(impulse, *frames))
            self.assertEqual(self.g["drive"][i], impulse[15])

    def test_fast_banks_match_the_generator(self):
        for i, impulse in enumerate(self.g["impulses"]):
            want = gen.fast_rows(impulse[15], self.g["white_run"][i], self.g["dark"][i], self.g["frames_dark"][i],
                                 self.g["light"][i], self.g["frames_light"][i])
            got = ["    LUT_MAKE(%s)," % ", ".join(str(c) for c in row) for row in self.g["fast"][i]]
            self.assertEqual(got, want, "range %d" % i)

    def test_greys_land_the_same_from_every_start(self):
        # Self-normalizing: the run up is as long as it takes this panel to get
        # from black to white (GRAY_WHITE_RUN), so on that scale, whatever the
        # pixel showed, the column leaves it k frames below white.
        for i in range(7):
            white = self.g["white_run"][i]
            for level, k in ((self.g["dark"][i], self.g["frames_dark"][i]),
                             (self.g["light"][i], self.g["frames_light"][i])):
                for start in range(white + 1):
                    self.assertEqual(land(self.g["fast"][i], level, start, white), white - k)

    def test_rails_are_the_vendor_du(self):
        for i, impulse in enumerate(self.g["impulses"]):
            bank = self.g["fast"][i]
            self.assertEqual(len(bank), impulse[15])
            self.assertTrue(all(row[0] == 1 and row[15] == 2 for row in bank))

    def test_budget(self):
        for i in range(7):
            total = (gen.ERASER_ROWS + 2 * gen.UNUSED_SLOT_ROWS + len(self.g["clean"][i]) + 1 +
                     len(self.g["fast"][i]) + 1)
            self.assertLessEqual(total, gen.LUT_ROW_BUDGET, "range %d" % i)


if __name__ == "__main__":
    unittest.main()
