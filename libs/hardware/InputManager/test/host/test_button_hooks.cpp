// The button hook list: a board's hook (setButtonHook) and further hooks beside it (addButtonHook), every
// installed mask OR'd into the raw state the debounce sees.
#include <Arduino.h>
#include <BoardConfig.h>
#include <Logging.h>
#include <Wire.h>
#include <driver/gpio.h>

#include <cassert>
#include <cstdio>

#define private public
#include <InputManager.h>
#undef private

namespace {
uint8_t boardHook() { return 1u << InputManager::BTN_DOWN; }
uint8_t heldHook() { return 1u << InputManager::BTN_BACK; }
uint8_t hookC() { return 1u << InputManager::BTN_LEFT; }
uint8_t hookD() { return 1u << InputManager::BTN_RIGHT; }
uint8_t hookE() { return 1u << InputManager::BTN_UP; }
constexpr uint8_t DOWN = 1u << InputManager::BTN_DOWN;
constexpr uint8_t BACK = 1u << InputManager::BTN_BACK;
}  // namespace

int main() {
  assert(!InputManager::hasButtonHook());
  assert(InputManager::readButtonHooks() == 0);

  // The board installs its hook the way it always has.
  InputManager::setButtonHook(&boardHook);
  assert(InputManager::hasButtonHook());
  assert(InputManager::readButtonHooks() == DOWN);

  // A second hook joins it instead of taking its place.
  assert(InputManager::addButtonHook(&heldHook));
  assert(InputManager::readButtonHooks() == (DOWN | BACK));
  // Adding one again keeps a single copy; nullptr is refused.
  assert(InputManager::addButtonHook(&heldHook));
  assert(InputManager::s_buttonHookCount == 2);
  assert(!InputManager::addButtonHook(nullptr));

  // The list is fixed: a hook past MAX_BUTTON_HOOKS is refused and the others keep working.
  assert(InputManager::addButtonHook(&hookC));
  assert(InputManager::addButtonHook(&hookD));
  assert(InputManager::s_buttonHookCount == InputManager::MAX_BUTTON_HOOKS);
  assert(!InputManager::addButtonHook(&hookE));
  assert((InputManager::readButtonHooks() & (1u << InputManager::BTN_UP)) == 0);

  // The update path ORs them into the raw state, whatever else the board reads.
  InputManager input;
  InputManager::setButtonHook(nullptr);
  assert(InputManager::addButtonHook(&heldHook));
  assert((input.getState() & BACK) == BACK);

  // setButtonHook replaces every hook, as the single slot did; nullptr leaves none.
  InputManager::setButtonHook(&boardHook);
  assert(InputManager::s_buttonHookCount == 1 && InputManager::readButtonHooks() == DOWN);
  InputManager::setButtonHook(nullptr);
  assert(!InputManager::hasButtonHook() && InputManager::readButtonHooks() == 0);

  std::puts("button hooks: OK");
  return 0;
}
