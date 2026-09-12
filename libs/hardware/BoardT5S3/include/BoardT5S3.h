#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "BoardT5S3Pins.h"

namespace BoardT5S3 {

class ScopedI2CLock {
 public:
  ScopedI2CLock();
  ~ScopedI2CLock();

  ScopedI2CLock(const ScopedI2CLock&) = delete;
  ScopedI2CLock& operator=(const ScopedI2CLock&) = delete;

 private:
  bool locked_ = false;
};

void begin();
void beginI2C();

// The recursive mutex every PCA9535 / TPS65185 access in this board layer takes.
//
// Exposed because this board layer is NOT the only thing on the bus: the GT911,
// the PCF8563 and the fuel gauge are driven from other tasks, through the
// firmware's own I2C lock. Two mutexes over one Wire serialise nothing, and the
// EPD power hooks are the worst possible caller to leave outside the shared one
// -- LovyanGFX's panel task runs them on every refresh, so a collision there
// fails the power-up and the panel then clocks a whole frame out with its
// high-voltage rails down.
//
// So the firmware adopts THIS handle for its own lock rather than the two
// layers each keeping one. Valid from begin() onward (beginI2C() creates it).
// Returned rather than taken so the caller can hold it with its own RAII type.
SemaphoreHandle_t i2cMutexHandle();

void prepareSdBus();
void disableGpsLora();
bool pca9535Present();
bool readPca9535Pin(uint8_t pin, bool* high);
bool writePca9535Pin(uint8_t pin, bool high);
bool setPca9535PinMode(uint8_t pin, uint8_t mode);
bool readButton();

}  // namespace BoardT5S3
