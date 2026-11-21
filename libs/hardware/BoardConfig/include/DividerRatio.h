#pragma once

#include <cstdint>

namespace BoardConfig {

// A battery voltage divider's ratio, in thousandths (2.0f is 2000). Profiles
// and callers still write the float: every board profile is constexpr, so the
// conversion runs at compile time and the ADC battery read multiplies in
// integers, which the ESP32-C3 (no FPU) does without soft-float calls.
struct DividerRatio {
  uint16_t milli;
  // Implicit on purpose: `2.0f` in a profile or a BatteryMonitor call converts.
  constexpr DividerRatio(float ratio) : milli(static_cast<uint16_t>(ratio * 1000.0f + 0.5f)) {}
};

}  // namespace BoardConfig
