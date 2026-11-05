// CHSC6x (Murphy M3): every touch registers, however short, and a contact only reads as a long
// press once it has been held that long. Frames and timings are from device logs.
#include <cassert>
#include <cstdio>

#include <Wire.h>
#include <InputManager.h>

namespace {
constexpr int kIrqPin = 44;
constexpr unsigned long kSampleMs = 10;  // the firmware's input sampler cadence

std::vector<uint8_t> contactAt(uint8_t b4, uint8_t b5, uint8_t b6) {
  return {0x00, 0x00, 0x01, 0x80, b4, b5, b6, 0x0D, 0x10};
}
std::vector<uint8_t> liftAt(uint8_t b4, uint8_t b5, uint8_t b6) {
  return {0x00, 0x00, 0x00, 0x40, b4, b5, b6, 0x0D, 0x10};
}

InputManager in;
unsigned long longPressAt = 0;  // when the classifier fired, 0 = not yet
int taps = 0;
int releases = 0;

void sample() {
  fakeNow += kSampleMs;
  in.update();
  float nx = 0.0f;
  float ny = 0.0f;
  if (in.wasTouchLongPress(nx, ny)) {
    assert(longPressAt == 0 && "a contact long-presses once");
    longPressAt = fakeNow;
  }
  if (in.wasTouchTap(nx, ny)) taps++;
  if (in.wasTouchReleased()) releases++;
}

// One IRQ pulse, sampled the way the firmware samples it: low for a pass, then idle.
void pulseIrq() {
  gpioLevels[kIrqPin] = LOW;
  sample();
  gpioLevels[kIrqPin] = HIGH;
}

void idle(int passes = 10) {
  for (int i = 0; i < passes; ++i) sample();
}

void resetCounts() {
  longPressAt = 0;
  taps = 0;
  releases = 0;
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

  // The quickest tap with a contact frame, and the very first touch since boot: the contact is
  // readable for one sample, and it is gone by the time a delayed retry would look.
  Wire.frame = contactAt(0x64, 0x01, 0x8B);
  pulseIrq();
  assert(in.isTouchPressed());
  const auto point = in.getTouchPoint();
  assert(point.valid && point.x == 371 && point.y == 124);
  Wire.frame = liftAt(0x64, 0x01, 0x8B);
  sample();
  assert(!in.isTouchPressed() && "the lift frame ends the contact, not a timer");
  assert(taps == 1 && releases == 1);
  assert(longPressAt == 0 && "a tap is not a long press");

  // Tapping again keeps reading the live register: the two others hold old data.
  idle();
  resetCounts();
  Wire.frame = contactAt(0x7C, 0x01, 0x47);
  pulseIrq();
  assert(in.isTouchPressed());
  Wire.frame = liftAt(0x7C, 0x01, 0x47);
  sample();
  assert(taps == 1);

  // A touch over before the read: the IRQ fired and only its lift frame is left, but that frame
  // has coordinates nothing has seen yet.
  idle();
  resetCounts();
  Wire.frame = liftAt(0x8C, 0x01, 0x2C);
  pulseIrq();
  idle();
  assert(taps == 1 && releases == 1 && !in.isTouchPressed());
  assert(longPressAt == 0);

  // An IRQ over the lift frame already handled is no touch of its own...
  resetCounts();
  pulseIrq();
  idle();
  assert(taps == 0 && releases == 0 && !in.isTouchPressed());

  // ...but the contact it announces can still arrive while the driver retries.
  pulseIrq();
  Wire.frame = contactAt(0x70, 0x01, 0x40);
  sample();
  assert(in.isTouchPressed());
  Wire.frame = liftAt(0x70, 0x01, 0x40);
  sample();
  assert(taps == 1);

  // A hold with the controller's IRQs further apart than the hold-over: frames carry the contact,
  // and the long press waits for the full threshold.
  idle();
  resetCounts();
  Wire.frame = contactAt(0x6C, 0x01, 0x72);
  pulseIrq();
  const unsigned long downAt = fakeNow;
  while (fakeNow - downAt < 700) {
    if ((fakeNow - downAt) % 150 < kSampleMs) {
      pulseIrq();
    } else {
      sample();
    }
    assert(in.isTouchPressed() && "a resting finger stays one contact");
  }
  assert(longPressAt != 0 && "a real hold long-presses");
  assert(longPressAt - downAt >= 490 && "not before it has been held");
  Wire.frame = liftAt(0x6C, 0x01, 0x72);
  sample();
  assert(!in.isTouchPressed() && releases == 1);

  // A controller replaying its contact frame with no IRQ behind it must not hold a touch forever.
  idle();
  resetCounts();
  Wire.frame = contactAt(0x60, 0x01, 0x60);
  pulseIrq();
  idle(400);
  assert(!in.isTouchPressed() && releases == 1);

  std::puts("CHSC6x touch passed");
}
