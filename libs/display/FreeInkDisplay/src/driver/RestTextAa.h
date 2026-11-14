#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <utility>

#if defined(BOARD_HAS_PSRAM)
#include <esp_heap_caps.h>
#endif

#include "RestTextCodes.h"

namespace freeink {

// One-pass text that rests every pixel already at its target: black that stays black, white that stays white
// and gray that stays gray are not driven again; only pixels that change take a drive (to white, to black, or
// the gray row). The rule is RestTextCodes.h, as in the Murphy's single push and Free-Ink PR #214's SE0400
// pass, here with X4ProTextAa's interface so a driver can take either. No charge map: it needs the glass,
// not its history.
//
// Codes, as X4ProTextAa leaves them in low()/high() (a driver sends low() and high() as its two planes):
//   (high 1, low 1) to white   (high 1, low 0) rest   (high 0, low 1) gray   (high 0, low 0) to black
// After finish(), high() is the page's white plane (gray counts as black), as X4ProTextAa leaves it.
class RestTextAa {
 public:
  ~RestTextAa() { free(_storage); }
  RestTextAa() = default;
  RestTextAa(const RestTextAa&) = delete;
  RestTextAa& operator=(const RestTextAa&) = delete;

  // Six planes of widthBytes x height: the page's base, two staging planes that become the codes, the page's
  // gray, and the glass (B/W and gray).
  void allocate(bool enabled, uint16_t widthBytes, uint16_t height) {
    const uint32_t size = static_cast<uint32_t>(widthBytes) * height;
    if (size != _size) {
      free(_storage);
      _storage = nullptr;
    }
    _wb = widthBytes;
    _h = height;
    _size = size;
    _enabled = enabled && size != 0;
#if defined(BOARD_HAS_PSRAM)
    if (_enabled && !_storage)
      _storage = static_cast<uint8_t*>(heap_caps_malloc(size * PLANES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#endif
    if (_storage) {
      _glassBw = plane(4);
      _glassGray = plane(5);
      _pageGray = plane(3);
    }
    invalidate();
  }
  bool ready() const { return _enabled && _storage != nullptr; }
  bool active() const { return _active; }
  bool onPanel() const { return _onPanel; }
  const uint8_t* base() const { return _storage; }
  const uint8_t* low() const { return plane(1); }
  const uint8_t* high() const { return plane(2); }
  void invalidate() {
    _active = _onPanel = _history = false;
    _rows[0] = _rows[1] = 0;
    _failed = false;
  }
  void begin(const uint8_t* bw) {
    _active = ready() && bw;
    _rows[0] = _rows[1] = 0;
    _failed = false;
    if (_active) memcpy(_storage, bw, _size);
  }
  // begin() for planes that were staged first: they stay, only the base is taken.
  void beginStaged(const uint8_t* bw) {
    _active = ready() && bw;
    if (_active) memcpy(_storage, bw, _size);
  }
  // A frame this helper did not compose: what the glass holds is no longer known, staged planes stay.
  void dropHistory() { _active = _onPanel = _history = false; }
  // The glass holds `bw` and no gray, as after a full or half refresh of it.
  void glassIs(const uint8_t* bw) {
    if (!ready() || !bw) return;
    memcpy(_glassBw, bw, _size);
    memset(_glassGray, 0, _size);
    _history = true;
  }
  void stage(unsigned plane_, const uint8_t* data, uint16_t y, uint16_t rows) {
    if (!ready() || plane_ > 1) return;
    if (!_active && plane_ == 0 && y == 0) {
      _rows[0] = _rows[1] = 0;
      _failed = false;
    }
    if (!data || !rows || y != _rows[plane_] || y >= _h || rows > _h - y) {
      _failed = true;
      return;
    }
    memcpy(plane(1 + plane_) + static_cast<uint32_t>(y) * _wb, data, static_cast<uint32_t>(rows) * _wb);
    _rows[plane_] += rows;
  }
  bool planeComplete(unsigned plane_) const { return !_failed && _rows[plane_] == _h; }
  bool compose() {
    if (!_active || _failed || _rows[0] != _h || _rows[1] != _h) return false;
    uint8_t* lo = plane(1);
    uint8_t* hi = plane(2);
    for (uint32_t i = 0; i < _size; ++i) {
      const auto gray = static_cast<uint8_t>(lo[i] | hi[i]);
      // Without a record of the glass nothing rests: every pixel is driven to its target.
      const RestTextDrives d = _history ? restTextDrives(_storage[i], gray, _glassBw[i], _glassGray[i], true)
                                        : RestTextDrives{0, static_cast<uint8_t>(_storage[i] & ~gray), gray,
                                                         static_cast<uint8_t>(~_storage[i] & ~gray)};
      _pageGray[i] = gray;
      lo[i] = static_cast<uint8_t>(d.toWhite | d.toGray);
      hi[i] = static_cast<uint8_t>(d.rest | d.toWhite);
    }
    return true;
  }
  // The pass is on the glass: it holds the page now. The doses are X4ProTextAa's and mean nothing here.
  void finish(int, int, int, int) {
    uint8_t* hi = plane(2);
    for (uint32_t i = 0; i < _size; ++i) hi[i] = static_cast<uint8_t>(_storage[i] & ~_pageGray[i]);
    memcpy(_glassBw, _storage, _size);
    std::swap(_glassGray, _pageGray);
    _active = false;
    _history = _onPanel = true;
  }

 private:
  static constexpr uint32_t PLANES = 6;
  uint8_t* plane(unsigned n) const { return _storage + _size * n; }

  uint8_t* _storage = nullptr;
  uint8_t* _glassBw = nullptr;
  uint8_t* _glassGray = nullptr;
  uint8_t* _pageGray = nullptr;  // swaps with _glassGray after each pass
  uint32_t _size = 0;
  uint16_t _wb = 0, _h = 0;
  uint16_t _rows[2] = {};
  bool _enabled = false;
  bool _active = false, _onPanel = false, _history = false, _failed = false;
};

}  // namespace freeink
