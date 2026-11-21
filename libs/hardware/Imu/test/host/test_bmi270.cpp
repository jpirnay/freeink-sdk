#include <BoardConfig.h>
#include <Imu.h>
#include <Wire.h>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "../../src/Bmi270ConfigFile.h"
static void xyz(uint8_t address, uint8_t reg, int16_t x, int16_t y, int16_t z) {
  for (int16_t v : {x,y,z}) {
    Wire.regs[address][reg++] = uint8_t(v);
    Wire.regs[address][reg++] = uint16_t(v) >> 8;
  }
}
static void powerOn(uint8_t address) {  // power-on reset state
  Wire.devices[address] = true;
  Wire.regs[address] = {};
  Wire.regs[address][0x00] = 0x24;
  Wire.regs[address][0x7C] = 3;
  Wire.bmiConfig.clear();
  Wire.bmiUploads = 0;
}
static bool sampling(uint8_t a) {
  const auto& r = Wire.regs[a];
  return r[0x40] == 0xA8 && r[0x41] == 0 && r[0x42] == 0xA8 && r[0x43] == 2 && r[0x7C] == 0 && r[0x7D] == 6;
}
static void put(uint8_t address, uint8_t reg, int16_t v) {
  Wire.regs[address][reg] = uint8_t(v);
  Wire.regs[address][reg + 1] = uint16_t(v) >> 8;
}
// Every raw reading on one axis: the exact product rounded half away from zero,
// so never more than half a unit (plus the float's own rounding) from the float
// (g or dps, times 1000) the driver computed before Sample became integers.
static void sweep(Imu& imu, uint8_t address, uint8_t reg, int32_t Imu::Sample::*axis, long num, long den,
                  float oldPerLsb) {
  for (long raw = -32768; raw <= 32767; ++raw) {
    put(address, reg, int16_t(raw));
    Imu::Sample s{};
    assert(imu.read(s));
    const long got = s.*axis;
    assert(got == std::lround(double(raw) * num / den));
    const double old = double(int16_t(raw) * oldPerLsb) * 1000;  // plus the float's own rounding, 2^-24 of it
    assert(std::fabs(got - old) <= 0.5 + std::fabs(old) * 1e-7);
  }
}
int main() {
  Imu imu;
  Imu::Sample sample = {42,42,42,42,42,42};
  static_assert(BoardConfig::PAPER_MONO.sensors.imuType == BoardConfig::ImuType::Bmi270);
  static_assert(BoardConfig::PAPER_MONO.sensors.imuAddr == 0x68);
  static_assert(!BoardConfig::PAPER_MONO.sensors.imuSwapXY && BoardConfig::PAPER_MONO.sensors.imuFlipX &&
                BoardConfig::PAPER_MONO.sensors.imuFlipY);
  BoardConfig::ACTIVE.sensors = BoardConfig::PAPER_MONO.sensors;
  assert(!imu.begin() && !imu.present());
  powerOn(0x68);
  Wire.regs[0x68][0] = 0x26;  // BMI260 or anything else is not a BMI270
  assert(!imu.begin());
  // Cold part: begin() identifies it only — no upload, no writes, sleep() is free.
  powerOn(0x68);
  Wire.writes.clear();
  assert(imu.begin() && imu.present() && imu.hasGyroscope());
  assert(Wire.writes.empty() && Wire.bmiUploads == 0);
  assert(imu.sleep() && Wire.writes.empty() && !imu.read(sample) && sample.axMg == 42);
  // The first wake() streams the whole file in order, with a longer bus timeout.
  assert(imu.wake() && Wire.bmiUploads == 1 && !Wire.bmiBadOffset);
  assert(Wire.bmiConfig.size() == sizeof(freeink::bmi270cfg::CONFIG_FILE) &&
         std::memcmp(Wire.bmiConfig.data(), freeink::bmi270cfg::CONFIG_FILE, Wire.bmiConfig.size()) == 0);
  assert(Wire.uploadTimeoutMs == 50 && Wire.timeoutMs == 4);
  assert(Wire.regs[0x68][0x59] == 1 && sampling(0x68));
  // Accel then gyro from DATA_8 in one burst; ±2 g, ±500 dps; Paper Mono flips X and Y.
  xyz(0x68, 0x0C, 16384, -8192, 4096);
  xyz(0x68, 0x12, 16384, -32768, 655);
  assert(imu.read(sample) && Wire.lastRead == 0x0C);
  assert(sample.axMg == -1000 && sample.ayMg == 500 && sample.azMg == 250);
  assert(sample.gxMdps == -250000 && sample.gyMdps == 500000 && sample.gzMdps == 9995);  // 655 * 500/32768 dps
  Wire.shortRead = true;
  assert(!imu.read(sample) && sample.axMg == -1000);
  Wire.shortRead = false;
  // Z: Paper Mono flips X and Y only.
  sweep(imu, 0x68, 0x10, &Imu::Sample::azMg, 125, 2048, 1.0f / 16384.0f);
  sweep(imu, 0x68, 0x16, &Imu::Sample::gzMdps, 15625, 1024, 500.0f / 32768.0f);
  // Sleep powers both sensors down; wake restores them without another upload.
  assert(imu.sleep() && Wire.regs[0x68][0x7D] == 0 && Wire.regs[0x68][0x7C] == 1 && !imu.read(sample));
  Wire.failWriteReg = 0x7D;
  assert(!imu.wake() && !imu.read(sample));
  Wire.failWriteReg = -1;
  assert(imu.wake() && sampling(0x68) && Wire.bmiUploads == 1 && imu.read(sample));
  // A host reset that kept the IMU powered: begin() finds init_ok and samples at once.
  Wire.bmiUploads = 0;
  Wire.regs[0x68][0x7D] = 0;
  assert(imu.begin() && Wire.bmiUploads == 0 && sampling(0x68) && imu.read(sample));
  // read() without wake() after a cold begin() also loads the file.
  powerOn(0x68);
  assert(imu.begin() && imu.read(sample) && Wire.bmiUploads == 1 && sampling(0x68));
  // Upload refused or interrupted: wake() fails, the timeout is restored, a retry recovers.
  // Each failure leaves both sensors off and advanced power save back on, so sleep() has
  // nothing left to undo.
  powerOn(0x68);
  Wire.bmiRefuseInit = true;
  assert(imu.begin() && !imu.wake() && Wire.regs[0x68][0x7C] == 1 && Wire.regs[0x68][0x7D] == 0);
  assert(!imu.read(sample) && Wire.regs[0x68][0x7C] == 1 && Wire.regs[0x68][0x7D] == 0);
  assert(imu.sleep() && Wire.regs[0x68][0x7C] == 1);
  Wire.bmiRefuseInit = false;
  Wire.failWriteReg = 0x5E;
  assert(!imu.wake() && Wire.timeoutMs == 4 && Wire.regs[0x68][0x7C] == 1 && Wire.regs[0x68][0x7D] == 0);
  Wire.failWriteReg = -1;
  assert(imu.wake() && Wire.regs[0x68][0x21] == 1 && sampling(0x68));
  // SDO high: the part answers at 0x69 and nothing is written to 0x68.
  Wire.devices[0x68] = false;
  powerOn(0x69);
  Wire.writes.clear();
  assert(imu.begin() && imu.wake() && sampling(0x69));
  for (auto w : Wire.writes) assert(w.addr == 0x69);
  puts("BMI270 identity/address, deferred config upload, warm start, scaling, mount flips, failures and sleep/wake passed");
}
