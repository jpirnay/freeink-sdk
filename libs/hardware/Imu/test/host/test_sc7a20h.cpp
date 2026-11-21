#include <BoardConfig.h>
#include <Imu.h>
#include <Wire.h>
#include <cassert>
#include <cmath>
#include <cstdio>
static void chip(uint8_t address, uint8_t who = 0x11, uint8_t version = 0x28) {
  Wire.devices[address] = true;
  Wire.regs[address][0x0F] = who;
  Wire.regs[address][0x70] = version;
}
static void xyz(uint8_t address, uint8_t reg, int16_t x, int16_t y, int16_t z) {
  for (int16_t v : {x,y,z}) {
    Wire.regs[address][reg++] = uint8_t(v);
    Wire.regs[address][reg++] = uint16_t(v) >> 8;
  }
}
static void put(uint8_t address, uint8_t reg, int16_t v) {
  Wire.regs[address][reg] = uint8_t(v);
  Wire.regs[address][reg + 1] = uint16_t(v) >> 8;
}
// Every raw reading on one axis: the exact product rounded half away from zero,
// so never more than half a unit (plus the float's own rounding) from the float
// (g or dps, times 1000) the driver computed before Sample became integers.
static void sweep(Imu& imu, uint8_t address, uint8_t reg, int32_t Imu::Sample::*axis, long num, long den,
                  float oldPerLsb, int step = 1) {
  for (long raw = -32768; raw <= 32767; raw += step) {
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
#if !FREEINK_CAP_IMU
  assert(!imu.begin() && !imu.present() && !imu.hasGyroscope());
  assert(!imu.read(sample) && !imu.sleep() && !imu.wake());
  return 0;
#else
  static_assert(BoardConfig::METALIO_EINK4.sensors.imuType == BoardConfig::ImuType::Sc7a20h);
  static_assert(BoardConfig::METALIO_EINK4.sensors.imuAddr == 0x19);
  assert(!imu.read(sample) && !imu.sleep() && !imu.wake() && !imu.hasGyroscope());
  assert(!imu.begin());
  chip(0x19, 0x33); // A LIS3DH-compatible layout isn't sufficient identity.
  assert(!imu.begin());
  chip(0x19, 0x11, 0); // Older SC7A20 is not the H version.
  assert(!imu.begin());
  chip(0x19);
  Wire.failReadReg = 0x70;
  assert(!imu.begin());
  Wire.failReadReg = -1;
  assert(imu.begin() && imu.present() && !imu.hasGyroscope());
  assert(Wire.regs[0x19][0x1F] == 1 && Wire.regs[0x19][0x20] == 0x57);
  assert(Wire.regs[0x19][0x23] == 0x80); // BDU, little endian, ±2g; HR is in 0x1F!
  assert(Wire.regs[0x19][0x22] == 0 && Wire.regs[0x19][0x2E] == 0);
  assert(!imu.read(sample) && sample.axMg == 42); // No data-ready yet.
  Wire.regs[0x19][0x27] = 0x08;
  xyz(0x19, 0x28, 0x400F, -16384, 8192);
  assert(imu.read(sample) && Wire.lastRead == 0xA8);
  assert(sample.axMg == 1000 && sample.ayMg == -1000 && sample.azMg == 500);
  assert(sample.gxMdps == 0 && sample.gyMdps == 0 && sample.gzMdps == 0);
  Wire.shortRead = true;
  assert(!imu.read(sample) && sample.axMg == 1000);
  Wire.shortRead = false;
  Wire.failReadReg = 0x28;
  assert(!imu.read(sample) && sample.axMg == 1000);
  Wire.failReadReg = -1;
  Wire.failWriteReg = 0x20;
  assert(!imu.sleep());
  Wire.failWriteReg = -1;
  assert(imu.sleep() && Wire.regs[0x19][0x20] == 7 && !imu.read(sample));
  Wire.failWriteReg = 0x20;
  assert(!imu.wake() && !imu.read(sample));
  Wire.failWriteReg = -1;
  assert(imu.wake() && Wire.regs[0x19][0x20] == 0x57 && imu.read(sample));
  xyz(0x19, 0x28, -32768, 32752, -16);
  assert(imu.read(sample));
  assert(sample.axMg == -2000 && sample.ayMg == 1999 && sample.azMg == -1);  // 2047/1024 g, -1/1024 g
  sweep(imu, 0x19, 0x28, &Imu::Sample::axMg, 125, 2048, 1.0f / 16384, 16);  // the low four bits are not data
  assert(imu.sleep());
  // Each initialization write can fail; a retry must recover and never report present on failure.
  for (int reg : {0x20,0x1F,0x21,0x22,0x23,0x24,0x25,0x2E}) {
    Wire.failWriteReg = reg;
    assert(!imu.begin() && !imu.present() && !imu.read(sample));
    if (reg != 0x20) assert(Wire.regs[0x19][0x20] == 7);
    Wire.failWriteReg = -1;
    assert(imu.begin() && imu.sleep());
  }
  // Wrong primary identity must not receive sensor writes; alternate SA0 address works.
  chip(0x19, 0x33); chip(0x18);
  Wire.regs[0x18][0x57] = 0x04;
  Wire.writes.clear();
  assert(imu.begin() && Wire.regs[0x18][0x57] == 0x0C);
  for (auto w : Wire.writes) assert(w.addr != 0x19);
  Wire.regs[0x18][0x27] = 8;
  xyz(0x18, 0x28, 0, 16384, 0);
  assert(imu.read(sample) && sample.ayMg == 1000);
  assert(imu.sleep() && Wire.regs[0x18][0x20] == 7);
  assert(imu.wake() && Wire.regs[0x18][0x20] == 0x57);
  Wire.failWriteReg = 0x57;
  assert(!imu.begin() && Wire.regs[0x18][0x20] == 7);
  Wire.failWriteReg = -1;
  assert(imu.begin());
  // Existing gyro backends retain their scaling and lifecycle behavior.
  BoardConfig::ACTIVE.sensors.imuType = BoardConfig::ImuType::Qmi8658;
  BoardConfig::ACTIVE.sensors.imuAddr = 0x6B;
  Wire.devices[0x6A] = true; Wire.regs[0x6A][0] = 5;
  assert(imu.begin() && imu.hasGyroscope());
  xyz(0x6A, 0x35, 16384, -16384, 0); xyz(0x6A, 0x3B, 64, -64, 0);
  assert(imu.read(sample) && sample.axMg == 1000 && sample.gxMdps == 1000 && sample.gyMdps == -1000);
  sweep(imu, 0x6A, 0x35, &Imu::Sample::axMg, 125, 2048, 1.0f / 16384.0f);
  sweep(imu, 0x6A, 0x3B, &Imu::Sample::gxMdps, 125, 8, 1.0f / 64.0f);
  assert(imu.sleep() && imu.wake());
  BoardConfig::ACTIVE.sensors.imuType = BoardConfig::ImuType::Lsm6ds3;
  BoardConfig::ACTIVE.sensors.imuAddr = 0x6A;
  Wire.regs[0x6A][0x0F] = 0x6A;
  assert(imu.begin() && imu.hasGyroscope());
  xyz(0x6A, 0x28, 1000, -1000, 0); xyz(0x6A, 0x22, 1000, -1000, 0);
  assert(imu.read(sample) && sample.axMg == 61 && sample.gxMdps == 8750);
  sweep(imu, 0x6A, 0x28, &Imu::Sample::axMg, 61, 1000, 0.061f / 1000.0f);
  sweep(imu, 0x6A, 0x22, &Imu::Sample::gxMdps, 35, 4, 8.75f / 1000.0f);
  assert(imu.sleep() && imu.wake());
  puts("SC7A20H identity/address, setup, signed scaling, data readiness, failures and sleep/wake passed; gyro regressions passed; every raw reading scales as the float driver did, to half a unit");
#endif
}
