#pragma once
// GPIO driver for the Murphy M3 touch test. Shadows picco_stubs/driver/gpio.h, whose two hold calls it
// keeps, and records what InputManager::armTouchPulseWake() / touchPulseWakeDone() do to a pin: its
// interrupt type, whether its interrupt is enabled, whether it is armed as a wake. A level type with
// the interrupt enabled re-enters the ISR for as long as the line holds the level, so the stub counts
// every moment that state exists.
#include <cassert>

using gpio_num_t = int;

enum gpio_int_type_t {
  GPIO_INTR_DISABLE = 0,
  GPIO_INTR_POSEDGE = 1,
  GPIO_INTR_NEGEDGE = 2,
  GPIO_INTR_ANYEDGE = 3,
  GPIO_INTR_LOW_LEVEL = 4,
  GPIO_INTR_HIGH_LEVEL = 5,
};

struct GpioStubPin {
  gpio_int_type_t type = GPIO_INTR_DISABLE;
  bool intrEnabled = false;
  bool wakeEnabled = false;
};
inline GpioStubPin gpioStubPins[64];
inline int gpioStubLevelWithLiveIntr = 0;  // times a level type and an enabled interrupt met

inline bool gpioStubIsLevel(gpio_int_type_t type) { return type == GPIO_INTR_LOW_LEVEL || type == GPIO_INTR_HIGH_LEVEL; }
inline void gpioStubCheck(gpio_num_t pin) {
  if (gpioStubPins[pin].intrEnabled && gpioStubIsLevel(gpioStubPins[pin].type)) gpioStubLevelWithLiveIntr++;
}

inline void gpio_hold_dis(int) {}
inline void gpio_hold_en(int) {}

inline int gpio_set_intr_type(gpio_num_t pin, gpio_int_type_t type) {
  gpioStubPins[pin].type = type;
  gpioStubCheck(pin);
  return 0;
}
inline int gpio_intr_enable(gpio_num_t pin) {
  gpioStubPins[pin].intrEnabled = true;
  gpioStubCheck(pin);
  return 0;
}
inline int gpio_intr_disable(gpio_num_t pin) {
  gpioStubPins[pin].intrEnabled = false;
  return 0;
}
// As IDF's: a level wake also sets the pin's interrupt type, which is what makes the order matter.
inline int gpio_wakeup_enable(gpio_num_t pin, gpio_int_type_t type) {
  assert(gpioStubIsLevel(type) && "light-sleep GPIO wake takes a level type");
  gpioStubPins[pin].type = type;
  gpioStubPins[pin].wakeEnabled = true;
  gpioStubCheck(pin);
  return 0;
}
inline int gpio_wakeup_disable(gpio_num_t pin) {
  gpioStubPins[pin].wakeEnabled = false;
  return 0;
}
