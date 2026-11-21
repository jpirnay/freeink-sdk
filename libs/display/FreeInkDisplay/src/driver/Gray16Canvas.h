#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "esp_heap_caps.h"

// A host-painted 8-bit canvas (sRGB gray, 0 = black .. 255 = white) dithered onto a panel's 16 calibrated levels,
// for the drivers that answer PanelDriver::borrowGray8Canvas() with a 16-level image (SSD2677 Picco, UC8179 X4 Pro).
//
// Integer only: the canvas value goes to CIE L* through a table, is stretched onto the L* the panel spans (its
// black and white level), and Floyd-Steinberg places it on the nearest level, carrying the error in tenths of L*.
namespace freeink::gray16 {

// CIE L* of each sRGB gray value, in hundredths (0 .. 10000). test_gray16_canvas recomputes every entry from the
// sRGB and CIE formulas.
inline constexpr uint16_t kLightnessCenti[256] = {
    0,    27,   55,   82,   110,  137,  165,  192,  219,   247,  274,  302,  332,  364,  397,  432,  468,  506,  546,
    588,  632,  677,  725,  774,  825,  876,  926,  977,   1027, 1077, 1126, 1176, 1225, 1274, 1323, 1371, 1420, 1468,
    1516, 1564, 1611, 1659, 1706, 1753, 1800, 1847, 1894,  1940, 1987, 2033, 2079, 2125, 2170, 2216, 2262, 2307, 2352,
    2397, 2442, 2487, 2532, 2576, 2621, 2665, 2709, 2753,  2797, 2841, 2885, 2929, 2972, 3016, 3059, 3103, 3146, 3189,
    3232, 3275, 3318, 3360, 3403, 3445, 3488, 3530, 3572,  3615, 3657, 3699, 3741, 3782, 3824, 3866, 3907, 3949, 3990,
    4032, 4073, 4114, 4155, 4196, 4237, 4278, 4319, 4360,  4401, 4441, 4482, 4522, 4563, 4603, 4644, 4684, 4724, 4764,
    4804, 4844, 4884, 4924, 4964, 5003, 5043, 5083, 5122,  5162, 5201, 5241, 5280, 5319, 5359, 5398, 5437, 5476, 5515,
    5554, 5593, 5632, 5670, 5709, 5748, 5786, 5825, 5864,  5902, 5940, 5979, 6017, 6056, 6094, 6132, 6170, 6208, 6246,
    6284, 6322, 6360, 6398, 6436, 6474, 6511, 6549, 6587,  6624, 6662, 6699, 6737, 6774, 6812, 6849, 6887, 6924, 6961,
    6998, 7035, 7072, 7110, 7147, 7184, 7221, 7257, 7294,  7331, 7368, 7405, 7442, 7478, 7515, 7551, 7588, 7625, 7661,
    7698, 7734, 7770, 7807, 7843, 7879, 7916, 7952, 7988,  8024, 8060, 8097, 8133, 8169, 8205, 8241, 8276, 8312, 8348,
    8384, 8420, 8456, 8491, 8527, 8563, 8598, 8634, 8670,  8705, 8741, 8776, 8812, 8847, 8882, 8918, 8953, 8988, 9024,
    9059, 9094, 9129, 9164, 9200, 9235, 9270, 9305, 9340,  9375, 9410, 9445, 9480, 9515, 9549, 9584, 9619, 9654, 9688,
    9723, 9758, 9793, 9827, 9862, 9896, 9931, 9965, 10000,
};

// `canvas` (width x height, one byte per pixel, rows of `width`) dithered onto the levels whose L* (x10) is
// `levelTenths`, level 0 the lightest and level 15 the darkest, strictly falling. The result is written in place,
// 4 bits per pixel, two pixels a byte (high nibble = the left pixel): the layout displayGray16Image() takes.
// Without the heap for two error rows each pixel takes its nearest level, undithered.
inline void canvasToLevels(uint8_t* canvas, const uint16_t width, const uint16_t height,
                           const uint16_t (&levelTenths)[16]) {
  const int32_t white = levelTenths[0];
  const int32_t black = levelTenths[15];
  const size_t rowLen = static_cast<size_t>(width) + 2;
  auto* err = static_cast<int16_t*>(heap_caps_malloc(rowLen * 2 * sizeof(int16_t), MALLOC_CAP_8BIT));
  if (err) memset(err, 0, rowLen * 2 * sizeof(int16_t));
  int16_t* cur = err;
  int16_t* next = err ? err + rowLen : nullptr;
  for (uint32_t y = 0; y < height; ++y) {
    const uint32_t row = y * width;
    for (uint32_t x = 0; x < width; ++x) {
      // The gray's L* stretched onto black..white, in tenths: kLightnessCenti / 10000 of the span, rounded.
      const int32_t target = black + ((white - black) * kLightnessCenti[canvas[row + x]] + 5000) / 10000;
      const int32_t want = target + (cur ? cur[x + 1] : 0);
      uint8_t level = 0;
      int32_t best = INT32_MAX;
      for (uint8_t l = 0; l < 16; ++l) {
        const int32_t d = abs(want - static_cast<int32_t>(levelTenths[l]));
        if (d < best) best = d, level = l;
      }
      if (cur) {
        const int32_t e = want - levelTenths[level];
        cur[x + 2] = static_cast<int16_t>(cur[x + 2] + e * 7 / 16);
        next[x] = static_cast<int16_t>(next[x] + e * 3 / 16);
        next[x + 1] = static_cast<int16_t>(next[x + 1] + e * 5 / 16);
        next[x + 2] = static_cast<int16_t>(next[x + 2] + e / 16);
      }
      // In place: byte (row + x) / 2 lies before every canvas byte still to be read.
      uint8_t& out = canvas[(row + x) / 2];
      out = (x & 1) ? static_cast<uint8_t>((out & 0xF0) | level) : static_cast<uint8_t>(level << 4);
    }
    if (cur) {
      int16_t* t = cur;
      cur = next;
      next = t;
      memset(next, 0, rowLen * sizeof(int16_t));
    }
  }
  heap_caps_free(err);
}

}  // namespace freeink::gray16
