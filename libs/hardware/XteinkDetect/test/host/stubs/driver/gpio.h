#pragma once
using gpio_num_t = int;
void gpio_hold_dis(gpio_num_t);
inline void gpio_hold_en(gpio_num_t) {}
// BoardConfig::driveReleasingHold() sets the level before it releases the hold on the Picco (Free-Ink fcadb76).
inline void gpio_set_level(gpio_num_t, unsigned) {}
