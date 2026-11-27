#pragma once
using gpio_num_t=int;
// BoardConfig::driveReleasingHold() sets the level before it releases the hold on the Picco (Free-Ink fcadb76).
inline int gpio_set_level(gpio_num_t, unsigned int) { return 0; }
inline void gpio_hold_dis(int) {}
inline void gpio_hold_en(int) {}
