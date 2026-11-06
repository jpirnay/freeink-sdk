// CHSC6x (Murphy M3): every touch registers, however short, and a contact only reads as a long
// press once it has been held that long. Frames and timings are from device logs. The corner and
// gesture checks follow test_murphy_touch.cpp in Free-Ink/freeink-sdk#25 by mr-tbot.
#include <cassert>
#include <cstdio>

#include <Wire.h>
#include <InputManager.h>

namespace {
constexpr int kIrqPin = 44;
constexpr unsigned long kSampleMs = 10;  // the firmware's input sampler cadence

std::vector<uint8_t> downAt(uint8_t b4, uint8_t b5, uint8_t b6) {
  return {0x00, 0x00, 0x01, 0x00, b4, b5, b6, 0x0D, 0x10};
}
std::vector<uint8_t> contactAt(uint8_t b4, uint8_t b5, uint8_t b6) {
  return {0x00, 0x00, 0x01, 0x80, b4, b5, b6, 0x0D, 0x10};
}
std::vector<uint8_t> liftAt(uint8_t b4, uint8_t b5, uint8_t b6) {
  return {0x00, 0x00, 0x00, 0x40, b4, b5, b6, 0x0D, 0x10};
}
std::vector<uint8_t> contactRaw(uint8_t rawX, uint16_t rawY) {
  return contactAt(rawX, static_cast<uint8_t>(rawY >> 8), static_cast<uint8_t>(rawY));
}
std::vector<uint8_t> liftRaw(uint8_t rawX, uint16_t rawY) {
  return liftAt(rawX, static_cast<uint8_t>(rawY >> 8), static_cast<uint8_t>(rawY));
}

InputManager in;
unsigned long longPressAt = 0;  // when the classifier fired, 0 = not yet
int taps = 0;
int swipes = 0;
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
  float endX = 0.0f;
  float endY = 0.0f;
  if (in.wasSwipe(nx, ny, endX, endY)) swipes++;
  if (in.wasTouchReleased()) releases++;
}

// One report: the frame, and an IRQ pulse that starts and ends between two sampler passes. On the
// device it is 0.4 ms wide, about every 7 ms from press to lift.
void report(const std::vector<uint8_t>& frame) {
  Wire.frame = frame;
  gpioLevels[kIrqPin] = LOW;
  if (touchIsr) touchIsr();
  gpioLevels[kIrqPin] = HIGH;
  sample();
}

void idle(int passes = 10) {
  for (int i = 0; i < passes; ++i) sample();
}

void resetCounts() {
  longPressAt = 0;
  taps = 0;
  swipes = 0;
  releases = 0;
}

// A contact that moves `px` mapped pixels along X, which raw Y follows one to one, and lifts.
void drag(unsigned px) {
  idle();
  resetCounts();
  report(contactRaw(124, 100));
  report(contactRaw(124, static_cast<uint16_t>(100 + px)));
  report(liftRaw(124, static_cast<uint16_t>(100 + px)));
}
}  // namespace

int main() {
  // Keys idle high (active low), and pin 2 is the Down key here, not the Picco's power interrupt
  // that the shared stub guards.
  powerInterruptEnabled = false;
  gpioLevels[0] = gpioLevels[1] = gpioLevels[2] = HIGH;
  gpioLevels[kIrqPin] = HIGH;
  in.begin();
  assert(touchIsr != nullptr && "the IRQ is latched, a pulse can be shorter than a sample");
  assert(in.hasTouch());

  // A quick tap, 20 ms from press to lift, is the whole of what the controller says about it.
  report(downAt(0x64, 0x01, 0x8B));
  assert(in.isTouchPressed());
  const auto point = in.getTouchPoint();
  assert(point.valid && point.x == 371 && point.y == 124);
  report(contactAt(0x64, 0x01, 0x8B));
  report(liftAt(0x64, 0x01, 0x8B));
  assert(!in.isTouchPressed() && "the lift frame ends the contact, not a timer");
  assert(taps == 1 && releases == 1);
  assert(longPressAt == 0 && "a tap is not a long press");

  // The report that follows every lift is no touch of its own.
  report(liftAt(0x64, 0x01, 0x8B));
  idle();
  assert(taps == 1 && releases == 1);

  // The next tap reads the live register too: the two others would hold the last one.
  resetCounts();
  report(contactAt(0x7C, 0x01, 0x47));
  assert(in.isTouchPressed());
  report(liftAt(0x7C, 0x01, 0x47));
  assert(taps == 1);

  // Press down opens a contact, it does not end one: the start of a swipe is no tap.
  idle();
  resetCounts();
  report(downAt(0x38, 0x00, 0x9D));
  assert(in.isTouchPressed() && taps == 0 && releases == 0);
  report(contactAt(0x38, 0x00, 0x9D));
  report(contactAt(0x36, 0x01, 0x48));
  report(liftAt(0x36, 0x01, 0x48));
  assert(!in.isTouchPressed() && taps == 0 && releases == 1);

  // A finger brushing the glass just before it lands leaves a lift frame with coordinates of its
  // own, then the real press 60 ms on: that is one tap, not two.
  idle();
  resetCounts();
  report(liftAt(0xA9, 0x01, 0x8C));
  idle(6);
  assert(!in.isTouchPressed() && taps == 0 && releases == 0);
  report(contactAt(0xAC, 0x01, 0x70));
  report(liftAt(0xAC, 0x01, 0x70));
  assert(taps == 1);

  // A report caught in mid-write is skipped, not taken for the end of the touch.
  idle();
  resetCounts();
  report(contactAt(0x33, 0x01, 0x8A));
  report({0x00, 0x00, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});
  assert(in.isTouchPressed() && releases == 0);
  report(liftAt(0x33, 0x01, 0x8A));
  assert(taps == 1);

  // What sits in the register with no IRQ behind it is not read, so it cannot start a touch.
  idle();
  resetCounts();
  Wire.frame = contactAt(0xA0, 0x00, 0x80);
  idle();
  assert(!in.isTouchPressed() && releases == 0);

  // A hold is one contact, and the long press waits for the full threshold.
  resetCounts();
  report(contactAt(0x6C, 0x01, 0x72));
  const unsigned long downAtMs = fakeNow;
  while (fakeNow - downAtMs < 700) {
    report(contactAt(0x6C, 0x01, 0x72));
    assert(in.isTouchPressed() && "a resting finger stays one contact");
  }
  assert(longPressAt != 0 && "a real hold long-presses");
  assert(longPressAt - downAtMs >= 490 && "not before it has been held");
  report(liftAt(0x6C, 0x01, 0x72));
  assert(!in.isTouchPressed() && releases == 1);

  // If the lift frame is lost, the silence after it ends the touch.
  idle();
  resetCounts();
  report(contactAt(0x60, 0x01, 0x60));
  idle(20);
  assert(!in.isTouchPressed() && releases == 1);

  // The mount: raw X runs along the panel's short axis and raw Y along its long one. swapXY puts
  // mapped X on the long axis, then flipY turns the other.
  struct Corner {
    uint8_t rawX;
    uint16_t rawY;
    uint16_t x;
    uint16_t y;
  };
  for (const Corner& c : {Corner{24, 24, 0, 200}, Corner{24, 398, 374, 200}, Corner{224, 24, 0, 0},
                          Corner{224, 398, 374, 0}}) {
    idle();
    report(contactRaw(c.rawX, c.rawY));
    const auto corner = in.getTouchPoint();
    assert(corner.valid && corner.x == c.x && corner.y == c.y);
    report(liftRaw(c.rawX, c.rawY));
  }

  // The thresholds are tuned on 800x480 digitizers: scaled to this one, a swipe starts at 26 px. A
  // pixel either side of that, a move is a tap or a swipe, never both and never neither.
  drag(5);
  assert(taps == 1 && swipes == 0);
  drag(25);
  assert(taps == 1 && swipes == 0);
  drag(26);
  assert(taps == 0 && swipes == 1);
  drag(30);
  assert(taps == 0 && swipes == 1);

  // Two fast taps stay two: the lift frame ends the first, so the second is a contact of its own
  // inside the hold-over that only covers a lost lift. On a device, 91 ms apart.
  idle();
  resetCounts();
  report(contactRaw(124, 100));
  report(liftRaw(124, 100));
  idle(9);
  report(contactRaw(124, 100));
  report(liftRaw(124, 100));
  assert(taps == 2 && releases == 2);

  // GPIO0 is the Confirm key and the wake pin, and the profile names it as both. A press is Confirm.
  idle();
  gpioLevels[0] = LOW;
  idle(5);
  assert(in.isPressed(InputManager::BTN_CONFIRM));
  assert(!in.isPressed(InputManager::BTN_POWER));
  gpioLevels[0] = HIGH;
  idle(5);

  std::puts("CHSC6x touch passed");
}
