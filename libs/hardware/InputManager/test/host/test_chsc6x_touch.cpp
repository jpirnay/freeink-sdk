// CHSC6x (Murphy M3): a contact only reads as a long press once it has been held that long.
#include <cassert>
#include <cstdio>

#include <Wire.h>
#include <InputManager.h>

namespace {
constexpr int kIrqPin = 44;
constexpr unsigned long kSampleMs = 10;  // the firmware's input sampler cadence

// Captured on a Murphy M3: a finger on the bottom hint strip, then the frame that follows the lift.
const std::vector<uint8_t> kContactFrame = {0x00, 0x00, 0x01, 0x80, 0x64, 0x01, 0x8B, 0x0D, 0x10};
const std::vector<uint8_t> kLiftFrame = {0x00, 0x00, 0x00, 0x40, 0x64, 0x01, 0x8B, 0x0D, 0x10};

InputManager in;
unsigned long longPressAt = 0;  // when the classifier fired, 0 = not yet

void sample() {
  fakeNow += kSampleMs;
  in.update();
  float nx = 0.0f;
  float ny = 0.0f;
  if (in.wasTouchLongPress(nx, ny)) {
    assert(longPressAt == 0 && "a contact long-presses once");
    longPressAt = fakeNow;
  }
}

// One IRQ pulse, sampled the way the firmware samples it: low for a pass, then idle.
void pulseIrq() {
  gpioLevels[kIrqPin] = LOW;
  sample();
  gpioLevels[kIrqPin] = HIGH;
}

// The driver drops the first frame after an IRQ and reads again 24 ms later.
void touchDown() {
  Wire.frame = kContactFrame;
  longPressAt = 0;
  pulseIrq();
  while (!in.isTouchPressed()) sample();
}

void waitForRelease() {
  Wire.frame = kLiftFrame;
  while (in.isTouchPressed()) sample();
}
}  // namespace

int main() {
  // Keys idle high (active low), and pin 2 is the Down key here, not the Picco's power interrupt
  // that the shared stub guards.
  powerInterruptEnabled = false;
  gpioLevels[0] = gpioLevels[1] = gpioLevels[2] = HIGH;
  gpioLevels[kIrqPin] = HIGH;
  in.begin();
  assert(in.hasTouch());

  // A tap: the read straddles a millisecond tick, as every read on this bus does, and the contact
  // is over long before the threshold.
  touchDown();
  const auto point = in.getTouchPoint();
  assert(point.valid && point.x == 371 && point.y == 124);
  waitForRelease();
  float nx = 0.0f;
  float ny = 0.0f;
  assert(in.wasTouchReleased() && in.wasTouchTap(nx, ny));
  assert(longPressAt == 0 && "a tap is not a long press");

  // A hold: fresh IRQs keep the contact open, and the long press waits for the full threshold.
  sample();
  touchDown();
  const unsigned long downAt = fakeNow;
  while (fakeNow - downAt < 700) {
    pulseIrq();
    sample();
    sample();
  }
  assert(in.isTouchPressed());
  assert(longPressAt != 0 && "a real hold still long-presses");
  assert(longPressAt - downAt >= 490 && "not before it has been held");
  waitForRelease();

  std::puts("CHSC6x touch passed");
}
